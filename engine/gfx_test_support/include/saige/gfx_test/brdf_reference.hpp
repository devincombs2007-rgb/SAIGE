// CPU reference of the glTF 2.0 metallic-roughness BRDF, for tests.
//
// The renderer's shaders must reproduce these formulas; the reference exists so a GPU result can be compared against an
// independent implementation (and so the formulas themselves are checked for the properties a physically based BRDF must have:
// a normalized distribution, reciprocity, energy conservation). Terms follow the Khronos glTF 2.0 specification, appendix B:
//   D  Trowbridge-Reitz / GGX with alpha = roughness^2
//   V  height-correlated Smith visibility, which already includes the 1 / (4 n.l n.v) of the microfacet denominator
//   F  Schlick Fresnel
//   dielectric  = (1 - F) * diffuse + F * specular        (f0 = 0.04, i.e. index of refraction 1.5)
//   metal       = (f0_metal + (1 - f0_metal) * (1 - v.h)^5) * specular, with f0_metal = base color
//   material    = mix(dielectric, metal, metallic)
//   diffuse = base color / pi, specular = D * V
// Roughness is clamped to `min_roughness` so a perfectly smooth surface cannot produce an infinite or NaN lobe; the shader
// must use the same clamp (the value is part of the contract).
#pragma once

#include "saige/render/math.hpp"

#include <algorithm>
#include <cmath>

namespace saige::test::brdf {

using render::Float3;

inline constexpr float pi = 3.14159265358979323846f;
inline constexpr float min_roughness = 0.045f;
inline constexpr float dielectric_f0 = 0.04f;

[[nodiscard]] inline float alpha_from_roughness(float roughness) noexcept
{
    const float r = std::clamp(roughness, min_roughness, 1.0f);
    return r * r;
}

/// GGX normal distribution; integrates to 1 over the hemisphere with the cosine (projected-area) weight.
[[nodiscard]] inline float D_ggx(float n_dot_h, float alpha) noexcept
{
    const float a2 = alpha * alpha;
    const float d = n_dot_h * n_dot_h * (a2 - 1.0f) + 1.0f;
    return a2 / (pi * d * d);
}

/// Height-correlated Smith visibility V = G / (4 n.l n.v).
[[nodiscard]] inline float V_smith_ggx_correlated(float n_dot_l, float n_dot_v, float alpha) noexcept
{
    const float a2 = alpha * alpha;
    const float gv = n_dot_l * std::sqrt(n_dot_v * n_dot_v * (1.0f - a2) + a2);
    const float gl = n_dot_v * std::sqrt(n_dot_l * n_dot_l * (1.0f - a2) + a2);
    const float sum = gv + gl;
    return sum > 0.0f ? 0.5f / sum : 0.0f;
}

[[nodiscard]] inline float schlick_weight(float v_dot_h) noexcept
{
    const float m = std::clamp(1.0f - v_dot_h, 0.0f, 1.0f);
    const float m2 = m * m;
    return m2 * m2 * m;
}

[[nodiscard]] inline Float3 F_schlick(Float3 f0, float v_dot_h) noexcept
{
    const float w = schlick_weight(v_dot_h);
    return {f0.x + (1.0f - f0.x) * w, f0.y + (1.0f - f0.y) * w, f0.z + (1.0f - f0.z) * w};
}

struct Surface {
    Float3 base_color{1.0f, 1.0f, 1.0f};  ///< linear
    float metallic = 0.0f;
    float roughness = 1.0f;
};

/// BRDF value f(l, v) in 1/sr (without the n.l cosine), for unit vectors n, v, l. Zero when either direction is at or below the
/// horizon.
[[nodiscard]] inline Float3 evaluate(const Surface& s, Float3 n, Float3 v, Float3 l) noexcept
{
    const float n_dot_l = render::dot(n, l);
    const float n_dot_v = render::dot(n, v);
    if (n_dot_l <= 0.0f || n_dot_v <= 0.0f) {
        return {};
    }
    const Float3 h = render::normalize(v + l);
    const float n_dot_h = std::clamp(render::dot(n, h), 0.0f, 1.0f);
    const float v_dot_h = std::clamp(render::dot(v, h), 0.0f, 1.0f);
    const float alpha = alpha_from_roughness(s.roughness);
    const float specular = D_ggx(n_dot_h, alpha) * V_smith_ggx_correlated(n_dot_l, n_dot_v, alpha);
    const float metallic = std::clamp(s.metallic, 0.0f, 1.0f);

    const float f_dielectric = dielectric_f0 + (1.0f - dielectric_f0) * schlick_weight(v_dot_h);
    const Float3 f_metal = F_schlick(s.base_color, v_dot_h);
    const float inv_pi = 1.0f / pi;
    const auto channel = [&](float base, float metal_fresnel) {
        const float dielectric = (1.0f - f_dielectric) * base * inv_pi + f_dielectric * specular;
        const float metal = metal_fresnel * specular;
        return dielectric * (1.0f - metallic) + metal * metallic;
    };
    return {channel(s.base_color.x, f_metal.x), channel(s.base_color.y, f_metal.y), channel(s.base_color.z, f_metal.z)};
}

/// Radiance reflected from a single directional light of irradiance-per-unit-cosine `light` (radiance units times solid angle):
/// f(l, v) * light * n.l.
[[nodiscard]] inline Float3 shade_directional(const Surface& s, Float3 n, Float3 v, Float3 l, Float3 light) noexcept
{
    const Float3 f = evaluate(s, n, v, l);
    const float n_dot_l = std::max(render::dot(n, l), 0.0f);
    return {f.x * light.x * n_dot_l, f.y * light.y * n_dot_l, f.z * light.z * n_dot_l};
}

/// The split-sum environment BRDF: for a white uniform environment, a surface with specular reflectance f0 reflects
/// `f0 * scale + bias` of the light at a view with the given n.v. `scale` is the lobe's directional albedo with Fresnel
/// weight (1 - w) and `bias` the part with w = (1 - v.h)^5, so a metal with base color f0 reflects f0 * scale + bias.
struct EnvBrdf {
    float scale = 0.0f;
    float bias = 0.0f;
};

/// Integrates the specular lobe (D * V, as in `evaluate`) over the half vector, importance-weighted by the GGX distribution so
/// the narrow lobe of a smooth surface is resolved at any roughness. An independent numeric reference for whatever table or
/// fit the renderer uses.
[[nodiscard]] inline EnvBrdf integrate_env_brdf(float roughness, float n_dot_v, int nu = 192, int nphi = 192) noexcept
{
    const float n_dot_v_c = std::clamp(n_dot_v, 1e-4f, 1.0f);
    const Float3 v{std::sqrt(1.0f - n_dot_v_c * n_dot_v_c), 0.0f, n_dot_v_c};
    const double alpha = static_cast<double>(alpha_from_roughness(roughness));
    constexpr double two_pi = 6.283185307179586;
    double scale = 0.0;
    double bias = 0.0;
    for (int i = 0; i < nu; ++i) {
        const double u = (i + 0.5) / nu;
        const double theta_h = std::atan(alpha * std::sqrt(u / (1.0 - u)));
        for (int j = 0; j < nphi; ++j) {
            const double phi_h = (j + 0.5) / nphi * two_pi;
            const Float3 h{static_cast<float>(std::sin(theta_h) * std::cos(phi_h)), static_cast<float>(std::sin(theta_h) * std::sin(phi_h)),
                           static_cast<float>(std::cos(theta_h))};
            const float v_dot_h = render::dot(v, h);
            if (v_dot_h <= 0.0f) {
                continue;
            }
            const Float3 l = h * (2.0f * v_dot_h) - v;
            if (l.z <= 0.0f) {
                continue;
            }
            // dω_l = 4 (v.h) dω_h, and under the GGX CDF sampling D * cosθ_h * dω_h = du dφ / (2π): the D cancels.
            const double lobe = static_cast<double>(V_smith_ggx_correlated(l.z, n_dot_v_c, static_cast<float>(alpha))) * static_cast<double>(l.z) * 4.0 *
                                static_cast<double>(v_dot_h) / static_cast<double>(h.z) / (static_cast<double>(nu) * nphi);
            const double w = static_cast<double>(schlick_weight(v_dot_h));
            scale += lobe * (1.0 - w);
            bias += lobe * w;
        }
    }
    return {static_cast<float>(scale), static_cast<float>(bias)};
}

/// Ambient light reflected by a surface in a hemispherical environment: diffuse by the normal's ambient radiance and specular
/// (split-sum, lobe blur ignored) by the mirror direction's, both scaled by occlusion. `ab` is the environment BRDF for this
/// roughness and n.v. Mirrors `shade_ambient` in shaders/scene/pbr.hlsli.
[[nodiscard]] inline Float3 shade_ambient(const Surface& s, float occlusion, EnvBrdf ab, Float3 ambient_diffuse,
                                          Float3 ambient_specular) noexcept
{
    const float metallic = std::clamp(s.metallic, 0.0f, 1.0f);
    const auto channel = [&](float base, float diffuse_radiance, float specular_radiance) {
        const float f0 = dielectric_f0 * (1.0f - metallic) + base * metallic;
        return (base * (1.0f - metallic) * diffuse_radiance + (f0 * ab.scale + ab.bias) * specular_radiance) * occlusion;
    };
    return {channel(s.base_color.x, ambient_diffuse.x, ambient_specular.x),
            channel(s.base_color.y, ambient_diffuse.y, ambient_specular.y),
            channel(s.base_color.z, ambient_diffuse.z, ambient_specular.z)};
}

} // namespace saige::test::brdf
