// Ambient occlusion, pass 2: one full-screen triangle that multiplies the HDR scene color by the occlusion of pass 1, blurred over 5x5 pixels with weights that fall off with the
// distance between the pixels and with the difference in view depth (a neighbour across a depth edge, or in the sky, contributes nothing), which removes the 4x4 rotation pattern
// without bleeding occlusion over silhouettes. The sky pixel is copied unchanged. The CPU reference is `ao_blur` in render/ambient_occlusion.hpp, step for step.
//
// Bindings: set 0: 0 HDR scene color, 1 occlusion (R16F), 2 scene depth; all read with Load. Push constants (48 bytes): the same block as pass 1.
//
// @shader   SceneAoApply
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

[[vk::binding(0, 0)]] Texture2D<float4> g_color;
[[vk::binding(1, 0)]] Texture2D<float> g_ao;
[[vk::binding(2, 0)]] Texture2D<float> g_depth;

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

float4 PSMain(VSOutput input) : SV_Target0
{
    const int2 p = int2(input.position.xy);
    const int2 hi = int2(int(pc.width) - 1, int(pc.height) - 1);
    const float4 color = g_color.Load(int3(p, 0));
    const float d = g_depth.Load(int3(p, 0));
    if (d <= 0.0) {
        return color;
    }
    const float zc = pc.near_plane / d;
    float sum = 0.0, weight = 0.0;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            const int2 q = clamp(p + int2(dx, dy), int2(0, 0), hi);
            const float dq = g_depth.Load(int3(q, 0));
            if (dq <= 0.0) {
                continue;
            }
            const float zq = pc.near_plane / dq;
            const float spatial = 1.0 / (1.0 + float(dx * dx + dy * dy));
            const float range = saturate(1.0 - abs(zq - zc) / (0.5 * pc.radius));
            const float w = spatial * range;
            sum += w * g_ao.Load(int3(q, 0));
            weight += w;
        }
    }
    const float ao = weight > 0.0 ? sum / weight : 1.0;
    return float4(color.rgb * ao, color.a);
}
