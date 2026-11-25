// Bloom downsample: one full-screen triangle that writes the next (half-size) level of the bloom chain from a floating-point source, with the 13-tap filter of
// Jimenez ("Next Generation Post Processing in Call of Duty: Advanced Warfare"): twelve bilinear taps around the destination pixel and the center, weighted
//
//   center 0.125; the four corners (+-2, +-2) 0.03125 each; the four edges (0, +-2) and (+-2, 0) 0.0625 each; the four inner diagonals (+-1, +-1) 0.125 each
//
// (they sum to 1; offsets are in source texels from the destination pixel's center). This file is the plain permutation; `bloom_prefilter.hlsl` compiles it with
// SAIGE_BLOOM_PREFILTER = 1, which first applies the soft-knee threshold to every tap: with b the largest channel, a tap is scaled by
// max(q, b - threshold) / b, q = clamp(b - threshold + knee, 0, 2 knee)^2 / (4 knee + 1e-5), knee = threshold * knee fraction (an absolute width, a push constant).
// Every tap is cleaned first (NaN and negatives become 0, values above 65504 are clamped), so one bad pixel cannot spread through the chain. The CPU reference is
// bloom_downsample / bloom_prefilter_downsample in render/bloom.hpp, step for step.
//
// Bindings: set 0: 0 source texture, 1 sampler (linear, clamp to edge). Push constants (16 bytes): 1 / source size, threshold, absolute knee.
//
// @shader   SceneBloomDown
// @entry    VSMain vs
// @entry    PSMain ps

#ifndef SAIGE_BLOOM_PREFILTER
#define SAIGE_BLOOM_PREFILTER 0
#endif

struct BloomDownPush {
    float2 texel;      // 1 / source size
    float threshold;   // prefilter only
    float knee;        // prefilter only: absolute soft-knee width
};
[[vk::push_constant]] ConstantBuffer<BloomDownPush> pc;

[[vk::binding(0, 0)]] Texture2D<float4> g_src;
[[vk::binding(1, 0)]] SamplerState g_sampler;

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

float clean(float x)
{
    return (x != x) ? 0.0 : min(max(x, 0.0), 65504.0);
}

float3 tap(float2 uv, float2 offset)
{
    float3 c = g_src.SampleLevel(g_sampler, uv + offset * pc.texel, 0).rgb;
    c = float3(clean(c.x), clean(c.y), clean(c.z));
#if SAIGE_BLOOM_PREFILTER
    const float b = max(c.x, max(c.y, c.z));
    float q = clamp(b - pc.threshold + pc.knee, 0.0, 2.0 * pc.knee);
    q = q * q / (4.0 * pc.knee + 1e-5);
    c = c * (max(q, b - pc.threshold) / max(b, 1e-5));
#endif
    return c;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float2 uv = input.uv;
    const float3 e = tap(uv, float2(0, 0));
    const float3 corners = tap(uv, float2(-2, 2)) + tap(uv, float2(2, 2)) + tap(uv, float2(-2, -2)) + tap(uv, float2(2, -2));
    const float3 edges = tap(uv, float2(0, 2)) + tap(uv, float2(-2, 0)) + tap(uv, float2(2, 0)) + tap(uv, float2(0, -2));
    const float3 diagonals = tap(uv, float2(-1, 1)) + tap(uv, float2(1, 1)) + tap(uv, float2(-1, -1)) + tap(uv, float2(1, -1));
    return float4(e * 0.125 + corners * 0.03125 + edges * 0.0625 + diagonals * 0.125, 1.0);
}
