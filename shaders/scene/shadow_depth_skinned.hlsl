// The sun shadow pass for skinned models: shadow_depth.hlsl with the vertex skinned first. Same push constants (light view-projection and the instance's
// world matrix rows); the joint palette is set 0 binding 0 (this pipeline has no other bind group; model.hlsl's skinned stage has it at set 2). Depth only, no pixel shader.
//
// @shader   SceneShadowDepthSkinned
// @entry    VSMain vs

struct ShadowPush {
    column_major float4x4 light_view_proj;
    float4 model_row0;
    float4 model_row1;
    float4 model_row2;
};
[[vk::push_constant]] ConstantBuffer<ShadowPush> pc;

[[vk::binding(0, 0)]] cbuffer SkinPalette
{
    float4 skin_rows[768];
};

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(4)]] uint4 joints : BLENDINDICES;
    [[vk::location(5)]] float4 weights : BLENDWEIGHT;
};

float4 VSMain(VSInput input) : SV_Position
{
    float4 r0 = float4(0.0, 0.0, 0.0, 0.0), r1 = r0, r2 = r0;
    [unroll] for (int i = 0; i < 4; ++i) {
        const uint j = min(input.joints[i], 255u);
        const float w = input.weights[i];
        r0 += w * skin_rows[3u * j];
        r1 += w * skin_rows[3u * j + 1u];
        r2 += w * skin_rows[3u * j + 2u];
    }
    const float4 sp = float4(input.position, 1.0);
    const float3 skinned = float3(dot(r0, sp), dot(r1, sp), dot(r2, sp));
    const float4 p = float4(skinned, 1.0);
    const float3 world = float3(dot(pc.model_row0, p), dot(pc.model_row1, p), dot(pc.model_row2, p));
    return mul(pc.light_view_proj, float4(world, 1.0));
}
