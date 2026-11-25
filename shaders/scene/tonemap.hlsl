// Tone mapping: one full-screen triangle that maps a linear HDR color to a displayable linear color.
//
// Input is a floating-point scene color (RGBA16_FLOAT in the HDR path); output is linear light for the target. The target's
// own format does the display encoding: an *_SRGB target applies the sRGB curve on write, a UNORM target stores the value
// as is. Exposure (a scale, 2^EV) is applied first, then the operator. The operators and the clamps below are mirrored
// exactly by `render::tone_map` in tonemap.hpp, which the tests use as the reference.
//
//   0 Linear      saturate(color)                       identity inside [0, 1], clips above (the LDR look)
//   1 Reinhard    saturate(c (1 + c / w^2) / (1 + c))   extended; white point w maps to 1
//   2 ACES fitted (c (2.51 c + 0.03)) / (c (2.43 c + 0.59) + 0.14), saturated   (Narkowicz 2015 fit of the ACES curve)
//
// Non-finite and negative input never reaches the operator: NaN becomes 0, negatives become 0, and values above the largest
// finite half float (65504) are clamped to it, so one bad pixel cannot turn into a NaN that spreads through later filters.
//
// Bindings: set 0: 0 HDR texture, 1 sampler (nearest or linear, the caller's choice). Push constants (16 bytes): exposure
// scale, operator, white point.
//
// @shader   SceneToneMap
// @entry    VSMain vs
// @entry    PSMain ps

#ifndef SAIGE_BLOOM
#define SAIGE_BLOOM 0
#endif

struct ToneMapPush {
    float exposure;    // linear scale, 2^EV
    uint op;
    float white_point; // Reinhard only
    float pad;         // SAIGE_BLOOM: the bloom intensity
};
[[vk::push_constant]] ConstantBuffer<ToneMapPush> pc;

[[vk::binding(0, 0)]] Texture2D<float4> g_hdr;
[[vk::binding(1, 0)]] SamplerState g_sampler;
#if SAIGE_BLOOM
[[vk::binding(2, 0)]] Texture2D<float4> g_bloom;
[[vk::binding(3, 0)]] SamplerState g_bloom_sampler;  // linear: the bloom is half the size of the scene
#endif

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

float clean(float x)
{
    return (x != x) ? 0.0 : min(max(x, 0.0), 65504.0);
}

float3 tone_map(float3 c, uint op, float white)
{
    c = float3(clean(c.x), clean(c.y), clean(c.z));
    if (op == 1u) {
        const float w2 = white * white;
        return saturate(c * (1.0 + c / w2) / (1.0 + c));
    }
    if (op == 2u) {
        return saturate((c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14));
    }
    return saturate(c);
}

float4 PSMain(VSOutput input) : SV_Target0
{
#if SAIGE_BLOOM
    const float3 hdr = (g_hdr.Sample(g_sampler, input.uv).rgb + pc.pad * g_bloom.SampleLevel(g_bloom_sampler, input.uv, 0).rgb) * pc.exposure;
#else
    const float3 hdr = g_hdr.Sample(g_sampler, input.uv).rgb * pc.exposure;
#endif
    return float4(tone_map(hdr, pc.op, pc.white_point), 1.0);
}
