// Physically based shading shared by the scene shaders (docs/rendering/material-architecture.md).
//
// Three contracts: a material says what it is (`MaterialData` on the CPU, the constants and textures in model.hlsl),
// `evaluate_surface` turns it into a `SurfaceData` at one pixel, and the functions below light a `SurfaceData`. Lighting
// never reads a material, so a different light source or a ray-traced hit can reuse it.
//
// The terms follow the glTF 2.0 specification (appendix B) and mirror the CPU reference
// engine/gfx_test_support/include/saige/gfx_test/brdf_reference.hpp, which the tests compare against:
//   D  GGX with alpha = roughness^2, V  height-correlated Smith visibility, F  Schlick Fresnel,
//   dielectric = (1 - F) * diffuse + F * specular (f0 = 0.04), metal = F(base color) * specular, mixed by `metallic`.
// Roughness is clamped to MIN_ROUGHNESS (part of the contract: it keeps a mirror from producing an infinite lobe).
//
// Light units: a directional light's color is the radiance that a white Lambertian surface facing it reflects (the 0.1
// and 0.2 convention). The BRDF's irradiance is therefore color * PI, so Lambert's base_color / PI gives base_color * color.
// Ambient light is a hemispherical gradient given as the same kind of radiance: a white diffuse surface reflects it as is.

#ifndef SAIGE_PBR_HLSLI
#define SAIGE_PBR_HLSLI

static const float PI = 3.14159265358979323846;
static const float MIN_ROUGHNESS = 0.045;
static const float DIELECTRIC_F0 = 0.04;

/// Everything lighting needs to know about a surface point, in linear units, with a unit world-space normal facing the viewer.
struct SurfaceData {
    float3 base_color;
    float alpha;
    float metallic;
    float roughness;  // perceptual, not clamped here
    float occlusion;  // multiplies ambient light only; 1 = unoccluded
    float3 normal;
    float3 emissive;
};

float D_GGX(float n_dot_h, float alpha)
{
    const float a2 = alpha * alpha;
    const float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float V_SmithGGXCorrelated(float n_dot_l, float n_dot_v, float alpha)
{
    const float a2 = alpha * alpha;
    const float gv = n_dot_l * sqrt(n_dot_v * n_dot_v * (1.0 - a2) + a2);
    const float gl = n_dot_v * sqrt(n_dot_l * n_dot_l * (1.0 - a2) + a2);
    const float sum = gv + gl;
    return sum > 0.0 ? 0.5 / sum : 0.0;
}

float schlick_weight(float v_dot_h)
{
    const float m = saturate(1.0 - v_dot_h);
    const float m2 = m * m;
    return m2 * m2 * m;
}

float3 F_Schlick(float3 f0, float v_dot_h)
{
    return f0 + (1.0 - f0) * schlick_weight(v_dot_h);
}

/// BRDF value f(l, v) in 1/sr, without the n.l cosine; zero when either direction is at or below the horizon.
float3 evaluate_brdf(SurfaceData s, float3 v, float3 l)
{
    const float3 n = s.normal;
    const float n_dot_l = dot(n, l);
    const float n_dot_v = dot(n, v);
    if (n_dot_l <= 0.0 || n_dot_v <= 0.0) {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 h = normalize(v + l);
    const float n_dot_h = saturate(dot(n, h));
    const float v_dot_h = saturate(dot(v, h));
    const float alpha = pow(clamp(s.roughness, MIN_ROUGHNESS, 1.0), 2.0);
    const float specular = D_GGX(n_dot_h, alpha) * V_SmithGGXCorrelated(n_dot_l, n_dot_v, alpha);
    const float metallic = saturate(s.metallic);

    const float f_dielectric = DIELECTRIC_F0 + (1.0 - DIELECTRIC_F0) * schlick_weight(v_dot_h);
    const float3 f_metal = F_Schlick(s.base_color, v_dot_h);
    const float3 dielectric = (1.0 - f_dielectric) * s.base_color / PI + f_dielectric * specular;
    const float3 metal = f_metal * specular;
    return lerp(dielectric, metal, metallic);
}

/// Radiance reflected from one directional light (`light_color`: see the units above).
float3 shade_directional(SurfaceData s, float3 v, float3 l, float3 light_color)
{
    return evaluate_brdf(s, v, l) * (light_color * PI) * max(dot(s.normal, l), 0.0);
}

/// Light from a uniform-hemisphere ambient environment. `ambient_diffuse` is its radiance in the normal's direction, and
/// `ambient_specular` in the mirror direction (a gradient sky differs between them). `env_brdf` is the split-sum table
/// entry for this surface's roughness and n.v (x scale on f0, y bias): src/env_brdf.cpp integrates it from the same terms as
/// above. Specular ignores the lobe's blur, which is exact for a uniform environment and an approximation otherwise (replaced
/// by prefiltered image-based lighting in M5). Occlusion scales both.
float3 shade_ambient(SurfaceData s, float2 env_brdf, float3 ambient_diffuse, float3 ambient_specular)
{
    const float metallic = saturate(s.metallic);
    const float3 diffuse_color = s.base_color * (1.0 - metallic);
    const float3 f0 = lerp(float3(DIELECTRIC_F0, DIELECTRIC_F0, DIELECTRIC_F0), s.base_color, metallic);
    return (diffuse_color * ambient_diffuse + (f0 * env_brdf.x + env_brdf.y) * ambient_specular) * s.occlusion;
}

#endif
