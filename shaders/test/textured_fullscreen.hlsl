// Test-only shader: a full-screen triangle that shows a texture, multiplied by a uniform tint.
// It exercises the RHI's bind groups (separate texture, sampler and uniform buffer bindings in set 0) with no vertex
// input. A 1:1 texture-to-target draw with a nearest sampler reproduces the texels exactly, and a sampler clamped to one
// mip level shows that level, which is how the tests verify upload and descriptor plumbing end to end.
//
// @shader   TestTextured
// @entry    VSMain vs
// @entry    PSMain ps

[[vk::binding(0, 0)]] Texture2D<float4> g_texture;
[[vk::binding(1, 0)]] SamplerState g_sampler;
[[vk::binding(2, 0)]] cbuffer Tint
{
    float4 g_tint;
};

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint id : SV_VertexID)
{
    // (0,0), (2,0), (0,2): one triangle that covers the whole viewport.
    const float2 p = float2((id << 1) & 2, id & 2);
    VSOutput output;
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    output.uv = float2(p.x, 1.0 - p.y); // uv (0,0) is the top-left texel; NDC +Y is up on screen
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    return g_texture.Sample(g_sampler, input.uv) * g_tint;
}
