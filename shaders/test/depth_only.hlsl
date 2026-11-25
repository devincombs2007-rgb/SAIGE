// Test-only shader: the vertex stage of a depth-only pass (a shadow-map pass has no pixel shader). Positions are transformed by a
// column-major matrix in push constants. Used to check, end to end, that depth-only pipelines work, that the rasterizer's depth bias
// reaches the stored depth, and that the result can be sampled with a comparison sampler (see shadow_probe.hlsl).
//
// @shader   TestDepthOnly
// @entry    VSMain vs

struct DepthPush {
    column_major float4x4 clip;
};
[[vk::push_constant]] ConstantBuffer<DepthPush> pc;

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
};

float4 VSMain(VSInput input) : SV_Position
{
    return mul(pc.clip, float4(input.position, 1.0));
}
