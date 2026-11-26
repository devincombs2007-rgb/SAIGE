// Local lights: point and spot (docs/rendering/local-lights.md). The shading function that the CPU reference
// (engine/render_reference/.../lights.hpp) defines and the GPU test compares against; it is shared by any shader that has a world position,
// a SurfaceData and a view direction. It does not read a material and it does not duplicate the BRDF: it computes the light's irradiance at the point
// and hands it to `evaluate_brdf` of pbr.hlsli, exactly as `shade_directional` does for the sun.
//
// Units: `intensity` is the irradiance, in lux, on a surface that faces the light squarely at 1 m (candela is lux at 1 m); the sun's convention is the same
// quantity (a directional light of color c gives irradiance c * PI). Falloff is inverse square, clamped at 1 cm; the influence is multiplied by
// saturate(1 - (d / R)^4)^2, exactly zero with zero slope at the radius R (so a light can be culled to a sphere of radius R without changing the picture).
// A spot multiplies by t^2 with t = saturate((cos a - cos outer) * scale), scale = 1 / max(cos inner - cos outer, 1e-4); a point light has no cone.

#ifndef SAIGE_LOCAL_LIGHTS_HLSLI
#define SAIGE_LOCAL_LIGHTS_HLSLI

#include "pbr.hlsli"

static const float MIN_LIGHT_DISTANCE = 0.01;

/// One light, 64 bytes (four float4: the layout of the light buffer, StructuredBuffer<float4> with four entries per light).
struct LocalLight {
    float4 position_radius;  // xyz: world position; w: radius in metres (influence is exactly zero at and beyond it)
    float4 color_intensity;  // rgb: linear hue; w: intensity (irradiance in lux at 1 m on a surface facing the light)
    float4 direction_outer;  // xyz: unit axis the light shines along (spot); w: cos(outer half angle), or -2 for a point light (no cone)
    float4 cone;             // x: cos(inner half angle); y: 1 / max(cos(inner) - cos(outer), 1e-4); zw unused
};

LocalLight load_local_light(StructuredBuffer<float4> lights, uint index)
{
    LocalLight l;
    l.position_radius = lights[index * 4u + 0u];
    l.color_intensity = lights[index * 4u + 1u];
    l.direction_outer = lights[index * 4u + 2u];
    l.cone = lights[index * 4u + 3u];
    return l;
}

float local_radius_window(float distance_to_light, float radius)
{
    const float x = distance_to_light / radius;
    const float x2 = x * x;
    const float w = saturate(1.0 - x2 * x2);
    return w * w;
}

float local_spot_cone(float cos_angle, float cos_outer, float scale)
{
    const float t = saturate((cos_angle - cos_outer) * scale);
    return t * t;
}

/// The light's irradiance (rgb, lux on a surface facing it squarely) at `world_pos`, and the unit direction from the point toward the light. Zero when the light does not reach the point.
float3 local_light_irradiance(LocalLight light, float3 world_pos, out float3 to_light)
{
    to_light = float3(0.0, 0.0, 1.0);
    const float3 d = light.position_radius.xyz - world_pos;
    const float dist = sqrt(dot(d, d));
    const float radius = light.position_radius.w;
    if (!(dist < radius)) {
        return float3(0.0, 0.0, 0.0);
    }
    if (dist > 1e-12) {
        to_light = d / dist;
    }
    const float clamped = max(dist, MIN_LIGHT_DISTANCE);
    float e = light.color_intensity.w * local_radius_window(dist, radius) / (clamped * clamped);
    if (light.direction_outer.w > -1.5) {
        const float cos_angle = -dot(to_light, light.direction_outer.xyz);
        e *= local_spot_cone(cos_angle, light.direction_outer.w, light.cone.y);   // zero at and outside the outer cone: t = saturate((cos a - cos outer) * scale) is 0 there
    }
    return light.color_intensity.rgb * e;
}

/// Radiance reflected from one local light: f(l, v) * E * max(n . l, 0).
float3 shade_local_light(SurfaceData s, float3 world_pos, float3 v, LocalLight light)
{
    float3 l;
    const float3 irradiance = local_light_irradiance(light, world_pos, l);
    return evaluate_brdf(s, v, l) * irradiance * max(dot(s.normal, l), 0.0);
}

#endif
