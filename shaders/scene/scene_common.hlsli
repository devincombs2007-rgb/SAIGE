// Shared declarations for the 0.1 scene shaders (owner: Rendering).
//
// Layout contract with C++: saige::render::DrawConstants in engine/render/include/saige/render/scene_renderer.hpp
// (static_assert-checked there). 128 bytes = the minimum push constant size every Vulkan device supports.
// World space follows docs/standards/coordinate-system.md: right-handed, +Z up, meters.
#ifndef SAIGE_SCENE_COMMON_HLSLI
#define SAIGE_SCENE_COMMON_HLSLI

struct DrawConstants {
    column_major float4x4 view_proj; // world -> clip (reverse-Z, infinite far plane)
    float4 position_yaw;             // cube: base position (xyz) + yaw in radians (w). ground: center (xyz) + half size (w)
    float4 color_scale;              // cube: linear albedo (rgb) + edge length in meters (a)
    float4 camera_fog;               // camera world position (xyz) + fog density per meter (w)
    float4 fog_color;                // linear sky/fog color (rgb), a unused
};

[[vk::push_constant]] ConstantBuffer<DrawConstants> pc;

static const float3 sun_direction = float3(0.36, 0.48, 0.80); // toward the sun, normalized
static const float3 sun_color = float3(1.00, 0.95, 0.85);

float3 apply_lighting(float3 albedo, float3 normal)
{
    const float n_dot_l = saturate(dot(normal, sun_direction));
    // Hemispherical ambient: sky above (+Z), warm ground bounce below.
    const float3 ambient = lerp(float3(0.20, 0.18, 0.16), pc.fog_color.rgb * 0.6, normal.z * 0.5 + 0.5);
    return albedo * (ambient + sun_color * n_dot_l);
}

float3 apply_fog(float3 color, float3 world_pos)
{
    const float distance_m = length(world_pos - pc.camera_fog.xyz);
    const float fog = 1.0 - exp(-distance_m * pc.camera_fog.w);
    return lerp(color, pc.fog_color.rgb, saturate(fog));
}

#endif // SAIGE_SCENE_COMMON_HLSLI
