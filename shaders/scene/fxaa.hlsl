// FXAA: one full-screen triangle that smooths the aliased edges of a finished (tone-mapped, display-referred) color image, in the compact form of Timothy Lottes' FXAA: a
// luma contrast test against the pixel's four diagonal neighbours (below the threshold the pixel is copied unchanged), then two bilinear blends of taps along the edge direction
// that the luma gradient implies, keeping the wider blend only if its luma stays inside the neighbourhood's range. The CPU reference is `fxaa_apply` in render/fxaa.hpp, step for step.
//
// Luma is computed from the *perceptual* value: the sampled color raised to `luma_exponent` (0.5 for a texture whose sRGB decode the sampler undid, 1 for a texture that already
// holds display values), weighted 0.299 / 0.587 / 0.114. The output is the blended color itself (the target's own format re-encodes it).
//
// Bindings: set 0: 0 color texture, 1 sampler (linear, clamp to edge). Push constants (32 bytes): 1 / size, edge threshold (relative), minimum edge threshold (absolute), span
// maximum (texels), luma exponent.
//
// @shader   SceneFxaa
// @entry    VSMain vs
// @entry    PSMain ps

struct FxaaPush {
    float2 texel;
    float edge_threshold;
    float edge_threshold_min;
    float span_max;
    float luma_exponent;
    float2 pad;
};
[[vk::push_constant]] ConstantBuffer<FxaaPush> pc;

[[vk::binding(0, 0)]] Texture2D<float4> g_color;
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

float3 fetch(float2 uv)
{
    return g_color.SampleLevel(g_sampler, uv, 0).rgb;
}

float luma(float3 c)
{
    const float3 p = pow(max(c, 0.0), pc.luma_exponent);
    return dot(p, float3(0.299, 0.587, 0.114));
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float2 uv = input.uv;
    const float3 rgb_m = fetch(uv);
    const float3 rgb_nw = fetch(uv + float2(-1.0, -1.0) * pc.texel);
    const float3 rgb_ne = fetch(uv + float2(1.0, -1.0) * pc.texel);
    const float3 rgb_sw = fetch(uv + float2(-1.0, 1.0) * pc.texel);
    const float3 rgb_se = fetch(uv + float2(1.0, 1.0) * pc.texel);
    const float luma_m = luma(rgb_m), luma_nw = luma(rgb_nw), luma_ne = luma(rgb_ne), luma_sw = luma(rgb_sw), luma_se = luma(rgb_se);
    const float luma_min = min(luma_m, min(min(luma_nw, luma_ne), min(luma_sw, luma_se)));
    const float luma_max = max(luma_m, max(max(luma_nw, luma_ne), max(luma_sw, luma_se)));
    if (luma_max - luma_min < max(pc.edge_threshold_min, luma_max * pc.edge_threshold)) {
        return float4(rgb_m, 1.0);  // no edge here: the pixel is unchanged
    }
    float2 dir;
    dir.x = -((luma_nw + luma_ne) - (luma_sw + luma_se));
    dir.y = (luma_nw + luma_sw) - (luma_ne + luma_se);
    const float dir_reduce = max((luma_nw + luma_ne + luma_sw + luma_se) * (0.25 * (1.0 / 8.0)), 1.0 / 128.0);
    const float rcp_dir_min = 1.0 / (min(abs(dir.x), abs(dir.y)) + dir_reduce);
    dir = clamp(dir * rcp_dir_min, float2(-pc.span_max, -pc.span_max), float2(pc.span_max, pc.span_max)) * pc.texel;
    const float3 rgb_a = 0.5 * (fetch(uv + dir * (1.0 / 3.0 - 0.5)) + fetch(uv + dir * (2.0 / 3.0 - 0.5)));
    const float3 rgb_b = rgb_a * 0.5 + 0.25 * (fetch(uv + dir * -0.5) + fetch(uv + dir * 0.5));
    const float luma_b = luma(rgb_b);
    return float4((luma_b < luma_min || luma_b > luma_max) ? rgb_a : rgb_b, 1.0);
}
