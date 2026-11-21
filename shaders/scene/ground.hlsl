// Procedural checkerboard ground plane at z = 0 (1 m squares), so motion is visible under a follow camera.
//
// @shader   SceneGround
// @entry    VSMain vs
// @entry    PSMain ps

#include "scene_common.hlsli"

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world_pos : TEXCOORD0;
};

VSOutput VSMain(uint vertex_id : SV_VertexID)
{
    // Two counter-clockwise triangles (seen from +Z) covering [-1, 1]^2.
    static const float2 corners[6] = {
        float2(-1, -1), float2(1, -1), float2(1, 1),
        float2(-1, -1), float2(1, 1), float2(-1, 1)
    };
    const float3 world = float3(pc.position_yaw.xy + corners[vertex_id] * pc.position_yaw.w, 0.0);

    VSOutput output;
    output.position = mul(pc.view_proj, float4(world, 1.0));
    output.world_pos = world;
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float2 cell = floor(input.world_pos.xy);
    const float checker = fmod(abs(cell.x + cell.y), 2.0);
    float3 albedo = lerp(float3(0.30, 0.31, 0.33), float3(0.46, 0.47, 0.49), checker);

    // Anti-aliased 1 m grid lines.
    const float2 grid = abs(frac(input.world_pos.xy - 0.5) - 0.5) / max(fwidth(input.world_pos.xy), 1e-4);
    const float line_mask = 1.0 - saturate(min(grid.x, grid.y) - 0.5);
    albedo = lerp(albedo, float3(0.18, 0.19, 0.20), line_mask * 0.6);

    const float3 lit = apply_lighting(albedo, float3(0, 0, 1));
    return float4(apply_fog(lit, input.world_pos), 1.0);
}
