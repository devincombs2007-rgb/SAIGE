// Ambient occlusion, pass 1: one full-screen triangle that writes, for each pixel, how open the surface is to its surroundings (1 = open, 0 = enclosed), from the scene depth alone.
//
// The scene depth is reverse-Z with an infinite far plane (device depth = near / view distance; 0 is the sky). Each pixel's view-space position is rebuilt from it, its normal from the
// neighbouring depths (the forward difference on the side with the smaller depth step, so a silhouette does not bend it), and twelve taps over a disc of `radius` meters (projected to
// pixels, clamped to 48; below a pixel every tap rounds onto the pixel itself, which occludes nothing) are compared with it in the manner of Alchemy's ambient obscurance: a tap in front of the surface plane (beyond `bias`) and within `radius` occludes in
// proportion to how directly above the surface it lies and how close it is. The taps' rotation follows a 4x4 ordered pattern that the apply pass's 5x5 blur removes. The CPU reference
// is `ao_generate` in render/ambient_occlusion.hpp, step for step.
//
// Bindings: set 0: 0 the scene depth (Load only; no sampler). Push constants (48 bytes): size, near plane, tan(fov/2) x and y, radius, intensity, bias (meters), pixels per radian-ish
// focal length (height / 2 / tan_y).
//
// @shader   SceneAoGenerate
// @entry    VSMain vs
// @entry    PSMain ps

struct AoPush {
    float width;
    float height;
    float near_plane;
    float tan_x;
    float tan_y;
    float radius;
    float intensity;
    float bias;
    float focal_px;
    float pad0;
    float pad1;
    float pad2;
};
[[vk::push_constant]] ConstantBuffer<AoPush> pc;

[[vk::binding(0, 0)]] Texture2D<float> g_depth;

struct VSOutput {
    float4 position : SV_Position;
};

VSOutput VSMain(uint id : SV_VertexID)
{
    const float2 p = float2((id << 1) & 2, id & 2);
    VSOutput output;
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

static const float sky_z = 1.0e6;
static const float bayer[16] = {0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0};
static const float golden_angle = 2.3999632;
static const float two_pi = 6.2831853;

float view_z(float d)
{
    return d > 0.0 ? pc.near_plane / d : sky_z;
}

float depth_at(int2 p)
{
    const int2 q = clamp(p, int2(0, 0), int2(int(pc.width) - 1, int(pc.height) - 1));
    return g_depth.Load(int3(q, 0));
}

float3 view_pos(int2 p, float z)
{
    const float u = (float(p.x) + 0.5) / pc.width;
    const float v = (float(p.y) + 0.5) / pc.height;
    return float3((2.0 * u - 1.0) * pc.tan_x * z, (1.0 - 2.0 * v) * pc.tan_y * z, -z);
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const int2 p = int2(input.position.xy);
    const float d = depth_at(p);
    if (d <= 0.0) {
        return float4(1.0, 0.0, 0.0, 1.0);  // sky
    }
    const float zc = view_z(d);
    const float3 pc3 = view_pos(p, zc);
    const float zr = view_z(depth_at(p + int2(1, 0))), zl = view_z(depth_at(p + int2(-1, 0)));
    const float zd = view_z(depth_at(p + int2(0, 1))), zu = view_z(depth_at(p + int2(0, -1)));
    const float3 dx = abs(zr - zc) < abs(zc - zl) ? view_pos(p + int2(1, 0), zr) - pc3 : pc3 - view_pos(p + int2(-1, 0), zl);
    const float3 dy = abs(zd - zc) < abs(zc - zu) ? view_pos(p + int2(0, 1), zd) - pc3 : pc3 - view_pos(p + int2(0, -1), zu);
    const float3 nc = cross(dy, dx);
    const float nl = length(nc);
    const float3 n = nl > 0.0 ? nc / nl : float3(0.0, 0.0, 1.0);

    const float r_px = min(pc.radius * pc.focal_px / zc, 48.0);
    const float phi = two_pi * bayer[(p.y & 3) * 4 + (p.x & 3)] / 16.0;
    const float r2 = pc.radius * pc.radius;
    float occ = 0.0;
    for (int i = 0; i < 12; ++i) {
        const float t = (float(i) + 0.5) / 12.0;
        const float a = phi + float(i) * golden_angle;
        const int2 s = p + int2(int(floor(cos(a) * t * r_px + 0.5)), int(floor(sin(a) * t * r_px + 0.5)));
        const float ds = depth_at(s);
        if (ds <= 0.0) {
            continue;  // the sky occludes nothing
        }
        const float3 v = view_pos(clamp(s, int2(0, 0), int2(int(pc.width) - 1, int(pc.height) - 1)), view_z(ds)) - pc3;
        const float d2 = dot(v, v);
        const float f = max(0.0, dot(v, n) - pc.bias);
        const float w = saturate(1.0 - d2 / r2);
        occ += f * w * pc.radius / (d2 + 0.01 * r2);
    }
    const float ao = saturate(1.0 - 2.0 * pc.intensity * occ / 12.0);
    return float4(ao, 0.0, 0.0, 1.0);
}
