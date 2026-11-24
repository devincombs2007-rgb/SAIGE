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
    float4 right;     // xyz: camera right * tan(fov / 2) * aspect, w: unused
    float4 up;        // xyz: camera up * tan(fov / 2), w: exposure
    float4 forward;   // xyz: camera forward, w: sun intensity
    float4 to_sun;    // xyz: unit vector toward the sun, w: Mie anisotropy g
    float4 geometry;  // x: planet radius, y: radius of the top of the atmosphere, z: Rayleigh scale height, w: Mie scale height (all km)
    float4 beta_r;    // xyz: Rayleigh coefficients per km, w: Mie coefficient per km
    float4 misc;      // x: 1 to apply 1 - exp(-x); y: the observer's height above the ground (km)
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

float4 PSMain(VSOutput input) : SV_Target0
{
    const float3 d = normalize(pc.forward.xyz + pc.right.xyz * input.ndc.x + pc.up.xyz * input.ndc.y);
    float3 radiance = sky_radiance(d, normalize(pc.to_sun.xyz)) * pc.up.w;
    if (pc.misc.x > 0.5) {
        radiance = 1.0 - exp(-radiance);
    }
    return float4(radiance, 1.0);
}
