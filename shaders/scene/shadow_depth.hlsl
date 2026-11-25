// The sun shadow pass for imported models: depth only, no pixel shader. Each instance is transformed by its world matrix (three rows of
// a 3x4 affine, as in model.hlsl) and by the light's orthographic view-projection. The depth written is the shadow map that model.hlsl
// compares against (reverse-Z like the camera: 1 at the light-side near plane, so a greater depth is closer to the sun).
//
// Push constants (112 bytes): the light's view-projection and the instance's world matrix rows.
//
// @shader   SceneShadowDepth
// @entry    VSMain vs

struct ShadowPush {
    column_major float4x4 light_view_proj;
    float4 model_row0;
    float4 model_row1;
    float4 model_row2;
};
[[vk::push_constant]] ConstantBuffer<ShadowPush> pc;

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
};

float4 VSMain(VSInput input) : SV_Position
{
    const float4 p = float4(input.position, 1.0);
    const float3 world = float3(dot(pc.model_row0, p), dot(pc.model_row1, p), dot(pc.model_row2, p));
    return mul(pc.light_view_proj, float4(world, 1.0));
}
