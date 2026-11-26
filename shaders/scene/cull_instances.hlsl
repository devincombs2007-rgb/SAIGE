// GPU frustum culling of instances (docs/rendering/gpu-culling.md). One thread per instance: the instance's bounding sphere (a local-space centre and radius shared by
// all instances of a primitive, carried to world space by the instance's affine matrix and scaled by its largest axis scale) is tested against the six frustum planes exactly as the
// CPU does (`intersects_sphere`: culled when the signed distance to any plane is below minus the radius; a radius of zero is never culled), and the instances that pass append their
// matrix rows to a compacted buffer. The append position is an atomic counter that lives in the indirect draw arguments themselves (the instance count, word 1), so the
// draw that follows needs no readback: the host initialises the arguments to {index_count, 0, 0, 0, 0} and this pass fills in the instance count.
//
// The order of the compacted instances is not defined (it follows atomic arrival); the draws it feeds are opaque and depth-tested, so the picture does not depend on it.
//
// Bindings (set 0, all compute): 0 parameters (uniform), 1 input rows (storage, three float4 per instance), 2 output rows (storage, same layout), 3 draw arguments (storage, five uint).
//
// @shader   SceneCullInstances
// @entry    CSMain cs

struct CullParams {
    float4 planes[6];  // xyz: inward normal, w: offset; the CPU's Frustum planes, in the same order (left, right, bottom, top, near, far)
    float4 sphere;     // xyz: local-space centre, w: radius (0: never culled)
    uint4 counts;      // x: the number of instances
};
[[vk::binding(0, 0)]] ConstantBuffer<CullParams> params;
[[vk::binding(1, 0)]] StructuredBuffer<float4> in_rows;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> out_rows;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> args;

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= params.counts.x) {
        return;
    }
    const float4 r0 = in_rows[id.x * 3u + 0u];
    const float4 r1 = in_rows[id.x * 3u + 1u];
    const float4 r2 = in_rows[id.x * 3u + 2u];
    bool visible = true;
    if (params.sphere.w > 0.0) {
        const float3 local = params.sphere.xyz;
        const float3 center = float3(dot(r0.xyz, local) + r0.w, dot(r1.xyz, local) + r1.w, dot(r2.xyz, local) + r2.w);
        const float3 c0 = float3(r0.x, r1.x, r2.x);
        const float3 c1 = float3(r0.y, r1.y, r2.y);
        const float3 c2 = float3(r0.z, r1.z, r2.z);
        const float max_scale = max(length(c0), max(length(c1), length(c2)));
        const float radius = params.sphere.w * max_scale;
        [unroll] for (int p = 0; p < 6; ++p) {
            if (dot(params.planes[p].xyz, center) + params.planes[p].w < -radius) {
                visible = false;
            }
        }
    }
    if (visible) {
        uint slot;
        InterlockedAdd(args[1], 1u, slot);
        out_rows[slot * 3u + 0u] = r0;
        out_rows[slot * 3u + 1u] = r1;
        out_rows[slot * 3u + 2u] = r2;
    }
}
