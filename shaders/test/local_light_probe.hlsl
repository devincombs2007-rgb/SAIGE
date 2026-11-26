// Test-only compute shader for the local light shading function (shaders/scene/local_lights.hlsli): one thread per probe point sums shade_local_light over a list of lights. The test compares the
// result with the CPU reference (render_reference lights + the BRDF oracle) in plain C++, so a failure points at the shader function, not at the RHI.
//
// Set 0: binding 0 probes (storage, four float4 per probe: position + metallic, normal + roughness, view direction, base color), binding 1 lights (storage, four float4 per light, see LocalLight),
// binding 2 results (storage, one float4 per probe: rgb sum, w = the number of lights that reached the probe). Push constants: probe count, light count.
//
// @shader   TestLocalLightProbe
// @entry    CSMain cs

#include "../scene/local_lights.hlsli"

struct ProbePush {
    uint probe_count;
    uint light_count;
};
[[vk::push_constant]] ConstantBuffer<ProbePush> pc;

[[vk::binding(0, 0)]] StructuredBuffer<float4> probes;
[[vk::binding(1, 0)]] StructuredBuffer<float4> lights;
[[vk::binding(2, 0)]] RWStructuredBuffer<float4> results;

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= pc.probe_count) {
        return;
    }
    const float4 p0 = probes[id.x * 4u + 0u];
    const float4 p1 = probes[id.x * 4u + 1u];
    const float4 p2 = probes[id.x * 4u + 2u];
    const float4 p3 = probes[id.x * 4u + 3u];
    SurfaceData s;
    s.base_color = p3.xyz;
    s.alpha = 1.0;
    s.metallic = p0.w;
    s.roughness = p1.w;
    s.occlusion = 1.0;
    s.normal = p1.xyz;
    s.emissive = float3(0.0, 0.0, 0.0);
    float3 sum = float3(0.0, 0.0, 0.0);
    float reached = 0.0;
    for (uint i = 0u; i < pc.light_count; ++i) {
        const LocalLight light = load_local_light(lights, i);
        float3 l;
        const float3 e = local_light_irradiance(light, p0.xyz, l);
        if (any(e > 0.0)) {
            reached += 1.0;
        }
        sum += shade_local_light(s, p0.xyz, p2.xyz, light);
    }
    results[id.x] = float4(sum, reached);
}
