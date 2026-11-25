// The atmosphere's sky pass: a full-screen triangle behind everything, each pixel the single-scattering radiance toward its view direction. This is the GPU twin of
// saige::render::sky_radiance (engine/render/src/atmosphere.cpp): the same ray-marched integral with 16 samples along the view ray and 8 toward the sun, in
// kilometers and per-kilometer coefficients, in float where the reference is double (agreement is checked pixel by pixel in the Vulkan tests). See
// engine/render/include/saige/render/atmosphere.hpp for the model and its limits.
//
// Push constants (112 bytes): the camera basis scaled to the frustum, so a pixel's direction is forward + right * ndc.x + up * ndc.y, the sun, and the atmosphere.
// The output is the linear radiance times the exposure; with `misc.x` set (an LDR scene target) the curve 1 - exp(-x) maps it into [0, 1) because nothing
// downstream tone maps it. NDC +Y is up (the RHI contract).
//
// @shader   SceneSky
// @entry    VSMain vs
// @entry    PSMain ps

struct SkyConstants {
    float4 right;     // xyz: camera right * tan(fov / 2) * aspect, w: sun disc intensity (0 = no disc)
    float4 up;        // xyz: camera up * tan(fov / 2), w: exposure
    float4 forward;   // xyz: camera forward, w: sun intensity
    float4 to_sun;    // xyz: unit vector toward the sun, w: Mie anisotropy g
    float4 geometry;  // x: planet radius, y: radius of the top of the atmosphere, z: Rayleigh scale height, w: Mie scale height (all km)
    float4 beta_r;    // xyz: Rayleigh coefficients per km, w: Mie coefficient per km
    float4 misc;      // x: 1 to apply 1 - exp(-x); y: the observer's height above the ground (km); z: moon intensity (0 = no moon); w: star intensity (0 = no stars)
};
[[vk::push_constant]] ConstantBuffer<SkyConstants> pc;

static const uint VIEW_STEPS = 16;
static const uint LIGHT_STEPS = 8;
static const float PI = 3.14159265358979;

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 ndc : TEXCOORD0;
};

VSOutput VSMain(uint vertex_id : SV_VertexID)
{
    // One triangle that covers the screen: (-1, -1), (3, -1), (-1, 3), counter-clockwise.
    const float2 corner = float2((vertex_id << 1) & 2, vertex_id & 2);
    VSOutput output;
    output.ndc = corner * 2.0 - 1.0;
    output.position = float4(output.ndc, 0.0, 1.0);  // depth 0: the far plane of the reverse-Z camera; the pass tests and writes no depth
    return output;
}

/// Near and far distances along o + t d (d unit) to a sphere of radius r at the origin; false when it misses.
bool sphere_hits(float3 o, float3 d, float r, out float t0, out float t1)
{
    const float b = dot(o, d), c = dot(o, o) - r * r;
    const float disc = b * b - c;
    t0 = 0.0;
    t1 = 0.0;
    if (disc < 0.0) {
        return false;
    }
    const float s = sqrt(disc);
    t0 = -b - s;
    t1 = -b + s;
    return true;
}

float3 sky_radiance(float3 d, float3 sun)
{
    const float radius = pc.geometry.x, top = pc.geometry.y, hr = pc.geometry.z, hm = pc.geometry.w;
    const float3 beta_r = pc.beta_r.xyz;
    const float beta_m = pc.beta_r.w;
    const float3 origin = float3(0.0, 0.0, radius + pc.misc.y);

    float t0, t1;
    if (!sphere_hits(origin, d, top, t0, t1)) {
        return float3(0.0, 0.0, 0.0);
    }
    float t_max = t1;
    float p0, p1;
    if (sphere_hits(origin, d, radius, p0, p1) && p0 > 0.0) {
        t_max = min(t_max, p0);  // the planet blocks the ray
    }
    if (!(t_max > 0.0)) {
        return float3(0.0, 0.0, 0.0);
    }
    const float segment = t_max / (float)VIEW_STEPS;
    float od_r = 0.0, od_m = 0.0;
    float3 sum_r = float3(0.0, 0.0, 0.0), sum_m = float3(0.0, 0.0, 0.0);
    for (uint i = 0; i < VIEW_STEPS; ++i) {
        const float t = ((float)i + 0.5) * segment;
        const float3 p = origin + d * t;
        const float height = length(p) - radius;
        const float dr = exp(-height / hr) * segment, dm = exp(-height / hm) * segment;
        od_r += dr;
        od_m += dm;
        float l0, l1, s0, s1;
        if (!sphere_hits(p, sun, top, l0, l1)) {
            continue;
        }
        if (sphere_hits(p, sun, radius, s0, s1) && s0 > 0.0) {
            continue;  // in the planet's shadow
        }
        const float light_segment = l1 / (float)LIGHT_STEPS;
        float lod_r = 0.0, lod_m = 0.0;
        for (uint j = 0; j < LIGHT_STEPS; ++j) {
            const float u = ((float)j + 0.5) * light_segment;
            const float qh = length(p + sun * u) - radius;
            lod_r += exp(-qh / hr) * light_segment;
            lod_m += exp(-qh / hm) * light_segment;
        }
        const float3 tau = beta_r * (od_r + lod_r) + 1.1 * beta_m * (od_m + lod_m);
        const float3 attenuation = exp(-tau);
        sum_r += attenuation * dr;
        sum_m += attenuation * dm;
    }
    const float mu = dot(d, sun), g = pc.to_sun.w;
    const float phase_r = 3.0 / (16.0 * PI) * (1.0 + mu * mu);
    const float phase_m = 3.0 / (8.0 * PI) * ((1.0 - g * g) * (1.0 + mu * mu)) / ((2.0 + g * g) * pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    return pc.forward.w * (sum_r * beta_r * phase_r + sum_m * beta_m * phase_m);
}

// ---- celestial bodies (reference: sun_disc_radiance, moon_radiance, star_radiance in atmosphere.cpp, with the same constants) ----------------------------------------------------

static const float SUN_ANGULAR_RADIUS = 0.00465;
static const float MOON_ANGULAR_RADIUS = 0.0045;

/// The air's transmittance from the observer toward the unit direction `dir`: 0 below the planet's horizon, else exp(-optical depth) over 64 steps (the reference's).
float3 body_transmittance(float3 dir)
{
    const float radius = pc.geometry.x, top = pc.geometry.y, hr = pc.geometry.z, hm = pc.geometry.w;
    const float3 origin = float3(0.0, 0.0, radius + pc.misc.y);
    float s0, s1;
    if (sphere_hits(origin, dir, radius, s0, s1) && s0 > 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    float t0, t1;
    if (!sphere_hits(origin, dir, top, t0, t1)) {
        return float3(1.0, 1.0, 1.0);
    }
    const float segment = t1 / 64.0;
    float od_r = 0.0, od_m = 0.0;
    for (uint i = 0; i < 64; ++i) {
        const float h = length(origin + dir * (((float)i + 0.5) * segment)) - radius;
        od_r += exp(-h / hr) * segment;
        od_m += exp(-h / hm) * segment;
    }
    return exp(-(pc.beta_r.xyz * od_r + 1.1 * pc.beta_r.w * od_m));
}

/// The disc of angular radius `radius` around `center`, seen along `d`: limb darkening and a soft edge, from sin(theta) (acos has no precision left at half a degree).
float disc_profile(float3 d, float3 center, float radius)
{
    if (dot(d, center) <= 0.0) {
        return 0.0;
    }
    const float x = length(cross(d, center)) / sin(radius);
    if (!(x < 1.0)) {
        return 0.0;
    }
    const float mu = sqrt(max(0.0, 1.0 - x * x));
    const float limb = 1.0 - 0.6 * (1.0 - mu);
    const float t = saturate((x - 0.85) / 0.15);
    return limb * (1.0 - t * t * (3.0 - 2.0 * t));
}

uint hash_u(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float u01(uint h)
{
    return (float)(h >> 8) * (1.0 / 16777216.0);
}

float3 star_field(float3 d, float3 sun)
{
    const float night_t = saturate((sun.z + 0.0175) / -0.1745);
    const float night = night_t * night_t * (3.0 - 2.0 * night_t);
    const float up_t = saturate(d.z / 0.2);
    const float horizon = up_t * up_t * (3.0 - 2.0 * up_t);
    if (!(night > 0.0) || !(horizon > 0.0)) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 p = d * 64.0;
    const int3 cell = (int3)floor(p);
    const uint h = hash_u(asuint(cell.x) ^ hash_u((asuint(cell.y) + 0x9e3779b9u) ^ hash_u(asuint(cell.z) + 0x85ebca6bu)));
    if (!(u01(h) < 0.5)) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 center = (float3)cell + 0.2 + 0.6 * float3(u01(hash_u(h + 1u)), u01(hash_u(h + 2u)), u01(hash_u(h + 3u)));
    const float falloff = max(0.0, 1.0 - length(p - center) / 0.12);
    if (!(falloff > 0.0)) {
        return float3(0.0, 0.0, 0.0);
    }
    const float b = u01(hash_u(h + 4u)), tint = u01(hash_u(h + 5u));
    const float level = pc.misc.w * night * horizon * (0.3 + 0.7 * b * b) * falloff * falloff;
    return level * float3(1.0 + (0.7 - 1.0) * tint, 0.85 + (0.8 - 0.85) * tint, 0.7 + (1.0 - 0.7) * tint);
}

float3 celestial_bodies(float3 d, float3 sun)
{
    float3 sum = float3(0.0, 0.0, 0.0);
    if (pc.right.w > 0.0) {
        const float profile = disc_profile(d, sun, SUN_ANGULAR_RADIUS);
        if (profile > 0.0) {
            sum += body_transmittance(sun) * (pc.right.w * profile);
        }
    }
    if (pc.misc.z > 0.0) {
        const float profile = disc_profile(d, -sun, MOON_ANGULAR_RADIUS);
        if (profile > 0.0) {
            sum += body_transmittance(-sun) * (pc.misc.z * profile);
        }
    }
    if (pc.misc.w > 0.0) {
        sum += star_field(d, sun);
    }
    return sum;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float3 d = normalize(pc.forward.xyz + pc.right.xyz * input.ndc.x + pc.up.xyz * input.ndc.y);
    const float3 sun_direction = normalize(pc.to_sun.xyz);
    float3 radiance = (sky_radiance(d, sun_direction) + celestial_bodies(d, sun_direction)) * pc.up.w;
    if (pc.misc.x > 0.5) {
        radiance = 1.0 - exp(-radiance);
    }
    return float4(radiance, 1.0);
}
