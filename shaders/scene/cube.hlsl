// Lit cube drawn once per world instance (0.1 first-frame milestone).
//
// @shader   SceneCube
// @entry    VSMain vs
// @entry    PSMain ps

#include "scene_common.hlsli"

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION; // unit cube, centered on the origin
    [[vk::location(1)]] float3 normal : NORMAL;
};

struct VSOutput {
    float4 position : SV_Position;
    [[vk::location(0)]] float3 world_pos : TEXCOORD0;
    [[vk::location(1)]] float3 normal : TEXCOORD1;
};

VSOutput VSMain(VSInput input)
{
    const float s = sin(pc.position_yaw.w);
    const float c = cos(pc.position_yaw.w);
    const float3x3 yaw = float3x3(c, -s, 0, s, c, 0, 0, 0, 1); // rotation about +Z (counter-clockwise)
    const float edge = pc.color_scale.a;

    float3 local = input.position * edge;
    local.z += 0.5 * edge; // the instance position is the cube's base, so it rests on the ground
    const float3 world = mul(yaw, local) + pc.position_yaw.xyz;

    VSOutput output;
    output.position = mul(pc.view_proj, float4(world, 1.0));
    output.world_pos = world;
    output.normal = mul(yaw, input.normal);
    return output;
}

float4 PSMain(VSOutput input) : SV_Target0
{
    const float3 lit = apply_lighting(pc.color_scale.rgb, normalize(input.normal));
    return float4(apply_fog(lit, input.world_pos), 1.0);
}
