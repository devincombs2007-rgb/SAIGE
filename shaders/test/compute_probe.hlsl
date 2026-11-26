// Test-only compute shaders for the RHI compute tests (dispatch, indirect dispatch, storage images). Each entry is a separate pipeline with its own bind group
// layout. The results are compared with plain C++ on the CPU, so a failure points at the RHI path and not at the shader.
//
// @shader   TestComputeProbe
// @entry    FillMain cs
// @entry    ArgsMain cs
// @entry    ImageMain cs

struct FillPush {
    uint count;
    uint seed;
};
[[vk::push_constant]] ConstantBuffer<FillPush> pc;

// FillMain: data[i] = (i * 2654435761) ^ seed for i < count. Set 0: binding 0 is a storage buffer.
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> data;

[numthreads(64, 1, 1)]
void FillMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < pc.count) {
        data[id.x] = (id.x * 2654435761u) ^ pc.seed;
    }
}

// ArgsMain: writes the arguments of an indirect dispatch from a count the GPU reads: ceil(counts[0] / 64) workgroups along x. Set 0: binding 0 reads, binding 1 writes.
[[vk::binding(0, 0)]] StructuredBuffer<uint> counts;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> args;

[numthreads(1, 1, 1)]
void ArgsMain(uint3 id : SV_DispatchThreadID)
{
    args[0] = (counts[0] + 63u) / 64u;
    args[1] = 1u;
    args[2] = 1u;
}

// ImageMain: image[x, y] = (x / 15, y / 15, 0.5, 1) over 8x8 workgroups. Set 0: binding 0 is a storage image (RGBA32F).
[[vk::binding(0, 0)]] RWTexture2D<unorm float4> image;

[numthreads(8, 8, 1)]
void ImageMain(uint3 id : SV_DispatchThreadID)
{
    image[id.xy] = float4(float(id.x) / 15.0, float(id.y) / 15.0, 0.5, 1.0);
}
