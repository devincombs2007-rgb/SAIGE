// Clustered light culling (docs/rendering/local-lights.md, step 3). One thread per cluster: the light's sphere (world position and radius from the LocalLight record, carried to view space by the view matrix) is tested against the
// cluster's axis-aligned box exactly as the CPU reference does (render_reference light_culling: the squared distance from the centre to the box is at most radius^2), the lights that meet it are listed in ASCENDING light index,
// and the list is truncated at max_per_cluster with the lights that did not fit counted in `overflow`. A thread owns its cluster's output, so the result is deterministic (no atomics, no ordering by arrival).
//
// View space: x right, y up, z forward. Set 0 (all compute; four storage buffers, the portable limit of a pipeline): binding 0 the lights (four float4 per light, struct LocalLight in local_lights.hlsli: only position_radius is
// read), binding 1 the cluster boxes (two float4 per cluster: lo.xyz, hi.xyz), binding 2 the per-cluster summary (two uints per cluster: the count listed, then the overflow), binding 3 the indices (max_per_cluster uints per
// cluster; unused slots are written 0xFFFFFFFF).
// Push constants: cluster count, light count, max per cluster, then the first three rows of the view matrix (world to view) as three float4.
//
// @shader   SceneCullLights
// @entry    CSMain cs

struct CullPush {
    uint cluster_count;
    uint light_count;
    uint max_per_cluster;
    uint pad;
    float4 view_row0;
    float4 view_row1;
    float4 view_row2;
};
[[vk::push_constant]] ConstantBuffer<CullPush> pc;

[[vk::binding(0, 0)]] StructuredBuffer<float4> lights;
[[vk::binding(1, 0)]] StructuredBuffer<float4> boxes;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> summary;   // [2 * cluster]: count listed, [2 * cluster + 1]: lights that met the cluster but did not fit
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> indices;

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    const uint cluster = id.x;
    if (cluster >= pc.cluster_count) {
        return;
    }
    const float3 lo = boxes[cluster * 2u + 0u].xyz;
    const float3 hi = boxes[cluster * 2u + 1u].xyz;
    uint count = 0u;
    uint dropped = 0u;
    const uint base = cluster * pc.max_per_cluster;
    for (uint s = 0u; s < pc.max_per_cluster; ++s) {
        indices[base + s] = 0xFFFFFFFFu;
    }
    for (uint i = 0u; i < pc.light_count; ++i) {
        const float4 pr = lights[i * 4u];
        const float4 w = float4(pr.xyz, 1.0);
        const float3 c = float3(dot(pc.view_row0, w), dot(pc.view_row1, w), dot(pc.view_row2, w));
        const float3 d = max(max(lo - c, float3(0.0, 0.0, 0.0)), c - hi);
        if (dot(d, d) <= pr.w * pr.w) {
            if (count < pc.max_per_cluster) {
                indices[base + count] = i;
                ++count;
            } else {
                ++dropped;
            }
        }
    }
    summary[cluster * 2u] = count;
    summary[cluster * 2u + 1u] = dropped;
}
