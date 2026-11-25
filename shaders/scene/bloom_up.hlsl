// Bloom upsample: one full-screen triangle that writes U_i = D_i + scatter * tent(U_{i+1}). `fine` (D_i, same size as the target) is read 1:1; `coarse` (the next,
// smaller level) is filtered with a 3x3 tent of bilinear taps one coarse texel apart, weights [1 2 1; 2 4 2; 1 2 1] / 16. Nothing is blended into the target (blending
// into RGBA16_FLOAT is not part of the RHI's portable subset), so each level is its own texture. The CPU reference is bloom_combine in render/bloom.hpp.
//
// Bindings: set 0: 0 fine texture, 1 coarse texture, 2 sampler (linear, clamp to edge). Push constants (16 bytes): 1 / coarse size, scatter.
//
// @shader   SceneBloomUp
// @entry    VSMain vs
// @entry    PSMain ps

struct BloomUpPush {
    float2 coarse_texel;  // 1 / coarse size
    float scatter;
    float pad;
};
[[vk::push_constant]] ConstantBuffer<BloomUpPush> pc;

[[vk::binding(0, 0)]] Texture2D<float4> g_fine;
[[vk::binding(1, 0)]] Texture2D<float4> g_coarse;
[[vk::binding(2, 0)]] SamplerState g_sampler;

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint id : SV_VertexID)
{
    const float2 p = float2((id << 1) & 2, id & 2);
    VSOutput output;
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    output.uv = float2(p.x, 1.0 - p.y);
    return output;
}

float3 coarse_tap(float2 uv, float2 offset)
{
    return g_coarse.SampleLevel(g_sampler, uv + offset * pc.coarse_texel, 0).rgb;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float2 uv = input.uv;
    const float3 fine = g_fine.SampleLevel(g_sampler, uv, 0).rgb;
    const float3 corners = coarse_tap(uv, float2(-1, -1)) + coarse_tap(uv, float2(1, -1)) + coarse_tap(uv, float2(-1, 1)) + coarse_tap(uv, float2(1, 1));
    const float3 edges = coarse_tap(uv, float2(0, -1)) + coarse_tap(uv, float2(-1, 0)) + coarse_tap(uv, float2(1, 0)) + coarse_tap(uv, float2(0, 1));
    const float3 center = coarse_tap(uv, float2(0, 0));
    const float3 tent = (corners + edges * 2.0 + center * 4.0) * (1.0 / 16.0);
    return float4(fine + pc.scatter * tent, 1.0);
}
