// Test-only shaders for the RHI indirect draw tests: a vertex shader that reads a 2D position from the vertex buffer and shifts it right by one NDC unit per instance
// (so the instance count of an indirect command shows in the image), and a pixel shader that outputs one constant color from push constants. The tests draw small
// triangles in separate regions of a render target and look at which regions were covered.
//
// @shader   TestDrawProbe
// @entry    VSMain vs
// @entry    PSMain ps

struct Push {
    float4 color;
};
[[vk::push_constant]] ConstantBuffer<Push> pc;

struct VSInput {
    [[vk::location(0)]] float2 position : POSITION;
    uint instance : SV_InstanceID;
};

float4 VSMain(VSInput input) : SV_Position
{
    return float4(input.position.x + float(input.instance), input.position.y, 0.0, 1.0);
}

float4 PSMain() : SV_Target0
{
    return pc.color;
}
