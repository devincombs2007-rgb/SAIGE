// Test-only shader: a full-screen triangle that outputs one constant color (and alpha) from push constants. With blending
// enabled in the pipeline, the target ends up as the blend equation of that color and what was there before, which is how the
// tests verify blend state end to end with exact expected values.
//
// @shader   TestBlendColor
// @entry    VSMain vs
// @entry    PSMain ps

struct Push {
    float4 color;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    // (0,0), (2,0), (0,2): one triangle that covers the whole viewport.
    const float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * 2.0 - 1.0, 0.0, 1.0);
}

float4 PSMain() : SV_Target0
{
    return pc.color;
}
