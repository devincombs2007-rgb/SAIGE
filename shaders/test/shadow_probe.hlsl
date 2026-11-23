// Test-only shader: reads a depth texture the way a shadow lookup will, with a comparison sampler, and shows the result as a grey level.
// The reference depth of each pixel is base + uv.y * range, so a column of the picture is a ladder of references against one stored depth
// and the row where the answer flips is the stored depth, to the resolution of the ladder.
//
// @shader   TestShadowProbe
// @entry    VSMain vs
// @entry    PSMain ps

[[vk::binding(0, 0)]] Texture2D<float> g_shadow_map;
[[vk::binding(1, 0)]] SamplerComparisonState g_shadow_sampler;
[[vk::binding(2, 0)]] cbuffer Probe
{
    float4 g_probe; // x: reference at uv.y = 0, y: reference range across uv.y
};

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint id : SV_VertexID)
{
    const float2 p = float2((id << 1) & 2, id & 2);
    VSOutput output;
    output.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    output.uv = float2(p.x, 1.0 - p.y); // uv (0,0) is the top-left texel
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float visible = g_shadow_map.SampleCmpLevelZero(g_shadow_sampler, input.uv, g_probe.x + input.uv.y * g_probe.y);
    return float4(visible, visible, visible, 1.0);
}
