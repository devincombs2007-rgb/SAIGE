// Test-only shaders for the GPU culling tests: a vertex stage that places a small triangle with an instance's affine matrix read from the (compacted) row buffer at set 0 binding 0 and
// maps world x and y to clip space by a fixed scale, and a pixel stage with one constant colour. Overlapping instances therefore give the same pixels in any drawing order, so the
// image does not depend on the (undefined) order the culling pass compacts in.
//
// @shader   TestCullDrawProbe
// @entry    VSMain vs
// @entry    PSMain ps

[[vk::binding(0, 0)]] StructuredBuffer<float4> rows;

struct VSInput {
    [[vk::location(0)]] float2 position : POSITION;
    uint instance : SV_InstanceID;
};

float4 VSMain(VSInput input) : SV_Position
{
    const float4 r0 = rows[input.instance * 3u + 0u];
    const float4 r1 = rows[input.instance * 3u + 1u];
    const float4 r2 = rows[input.instance * 3u + 2u];
    const float4 p = float4(input.position, 0.0, 1.0);
    const float3 world = float3(dot(r0, p), dot(r1, p), dot(r2, p));
    return float4(world.x * 0.1, world.y * 0.1, 0.0, 1.0);
}

float4 PSMain() : SV_Target0
{
    return float4(1.0, 0.0, 0.0, 1.0);
}
