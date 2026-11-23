// CPU references for image-based lighting (milestone M5), for tests and for deciding the algorithms before any GPU work:
//  * order-2 spherical harmonics (9 coefficients per channel) of an environment, and the cosine-convolved diffuse irradiance from them
//    (Ramamoorthi and Hanrahan 2001: band factors 1, 2/3, 1/4 on the radiance coefficients);
//  * brute-force diffuse irradiance by quadrature over the sphere, the ground truth for the SH approximation;
//  * the split-sum GGX prefilter of an environment for a reflection direction and roughness, by importance sampling.
// An environment is any callable `Float3(Float3 direction)` returning linear radiance toward the viewer from that direction.
// "Diffuse radiance factor" below is irradiance / pi: what a white Lambertian surface with that normal reflects, the unit the
// renderer's ambient term already uses (a uniform environment of radiance c gives c).
#pragma once

#include "saige/render/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace saige::test::ibl {

using render::Float3;

inline constexpr double pi_d = 3.14159265358979323846;

struct Sh9 {
    std::array<Float3, 9> c{};  ///< radiance coefficients L_lm in the order 00, 1-1, 10, 11, 2-2, 2-1, 20, 21, 22
};

/// The nine real SH basis functions at a unit direction (z up).
[[nodiscard]] inline std::array<double, 9> sh_basis(Float3 d) noexcept
{
    const double x = static_cast<double>(d.x), y = static_cast<double>(d.y), z = static_cast<double>(d.z);
    return {0.282094791773878,
            0.488602511902920 * y,
            0.488602511902920 * z,
            0.488602511902920 * x,
            1.092548430592079 * x * y,
            1.092548430592079 * y * z,
            0.315391565252520 * (3.0 * z * z - 1.0),
            1.092548430592079 * x * z,
            0.546274215296040 * (x * x - y * y)};
}

namespace detail {
[[nodiscard]] inline Float3 direction(double theta, double phi) noexcept
{
    const double s = std::sin(theta);
    return {static_cast<float>(s * std::cos(phi)), static_cast<float>(s * std::sin(phi)), static_cast<float>(std::cos(theta))};
}
} // namespace detail

/// Projects `env` onto the SH basis by midpoint quadrature over a latitude-longitude grid (weights sin(theta)).
template <typename Env>
[[nodiscard]] Sh9 sh_project(Env&& env, int theta_steps = 256, int phi_steps = 512)
{
    double acc[9][3] = {};
    const double dt = pi_d / theta_steps, dp = 2.0 * pi_d / phi_steps;
    for (int i = 0; i < theta_steps; ++i) {
        const double theta = (i + 0.5) * dt;
        const double weight = std::sin(theta) * dt * dp;
        for (int j = 0; j < phi_steps; ++j) {
            const Float3 d = detail::direction(theta, (j + 0.5) * dp);
            const Float3 l = env(d);
            const std::array<double, 9> b = sh_basis(d);
            for (int k = 0; k < 9; ++k) {
                acc[k][0] += static_cast<double>(l.x) * b[static_cast<size_t>(k)] * weight;
                acc[k][1] += static_cast<double>(l.y) * b[static_cast<size_t>(k)] * weight;
                acc[k][2] += static_cast<double>(l.z) * b[static_cast<size_t>(k)] * weight;
            }
        }
    }
    Sh9 out;
    for (size_t k = 0; k < 9; ++k) {
        out.c[k] = {static_cast<float>(acc[k][0]), static_cast<float>(acc[k][1]), static_cast<float>(acc[k][2])};
    }
    return out;
}

/// Diffuse radiance factor (irradiance / pi) for normal `n` from SH coefficients: the radiance coefficients scaled by the clamped
/// cosine's band weights A_0 / pi = 1, A_1 / pi = 2/3, A_2 / pi = 1/4.
[[nodiscard]] inline Float3 sh_diffuse(const Sh9& sh, Float3 n) noexcept
{
    const std::array<double, 9> b = sh_basis(n);
    static constexpr double band[9] = {1.0, 2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25, 0.25};
    double r = 0.0, g = 0.0, bl = 0.0;
    for (size_t k = 0; k < 9; ++k) {
        r += static_cast<double>(sh.c[k].x) * b[k] * band[k];
        g += static_cast<double>(sh.c[k].y) * b[k] * band[k];
        bl += static_cast<double>(sh.c[k].z) * b[k] * band[k];
    }
    return {static_cast<float>(r), static_cast<float>(g), static_cast<float>(bl)};
}

/// Ground truth diffuse radiance factor by quadrature of (1/pi) * integral of L(l) max(n.l, 0) over the sphere.
template <typename Env>
[[nodiscard]] Float3 diffuse_brute(Env&& env, Float3 n, int theta_steps = 512, int phi_steps = 1024)
{
    double r = 0.0, g = 0.0, b = 0.0;
    const double dt = pi_d / theta_steps, dp = 2.0 * pi_d / phi_steps;
    for (int i = 0; i < theta_steps; ++i) {
        const double theta = (i + 0.5) * dt;
        const double weight = std::sin(theta) * dt * dp;
        for (int j = 0; j < phi_steps; ++j) {
            const Float3 d = detail::direction(theta, (j + 0.5) * dp);
            const double cosine = std::max(static_cast<double>(dot(n, d)), 0.0);
            if (cosine == 0.0) continue;
            const Float3 l = env(d);
            r += static_cast<double>(l.x) * cosine * weight;
            g += static_cast<double>(l.y) * cosine * weight;
            b += static_cast<double>(l.z) * cosine * weight;
        }
    }
    return {static_cast<float>(r / pi_d), static_cast<float>(g / pi_d), static_cast<float>(b / pi_d)};
}

[[nodiscard]] inline double radical_inverse(uint32_t bits) noexcept
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<double>(bits) * 2.3283064365386963e-10;
}

/// The split-sum prefiltered environment for a mirror direction `r` (taking the normal and view equal to it) and perceptual roughness:
/// the n.l weighted mean of L over GGX importance-sampled light directions (alpha = roughness^2). Deterministic (Hammersley points).
template <typename Env>
[[nodiscard]] Float3 ggx_prefilter(Env&& env, Float3 r, float roughness, uint32_t samples = 4096)
{
    const double alpha = std::max(static_cast<double>(roughness), 0.045) * std::max(static_cast<double>(roughness), 0.045);
    const Float3 helper = std::fabs(r.z) < 0.999f ? Float3{0.0f, 0.0f, 1.0f} : Float3{1.0f, 0.0f, 0.0f};
    const Float3 tx = normalize(cross(helper, r));
    const Float3 ty = cross(r, tx);
    double sr = 0.0, sg = 0.0, sb = 0.0, total = 0.0;
    for (uint32_t i = 0; i < samples; ++i) {
        const double u1 = (static_cast<double>(i) + 0.5) / static_cast<double>(samples), u2 = radical_inverse(i);
        const double phi = 2.0 * pi_d * u2;
        const double cos_theta = std::sqrt((1.0 - u1) / (1.0 + (alpha * alpha - 1.0) * u1));
        const double sin_theta = std::sqrt(std::max(0.0, 1.0 - cos_theta * cos_theta));
        const Float3 h = tx * static_cast<float>(sin_theta * std::cos(phi)) + ty * static_cast<float>(sin_theta * std::sin(phi)) +
                         r * static_cast<float>(cos_theta);
        const Float3 l = h * (2.0f * dot(r, h)) - r;
        const double n_dot_l = static_cast<double>(dot(r, l));
        if (n_dot_l <= 0.0) continue;
        const Float3 radiance = env(l);
        sr += static_cast<double>(radiance.x) * n_dot_l;
        sg += static_cast<double>(radiance.y) * n_dot_l;
        sb += static_cast<double>(radiance.z) * n_dot_l;
        total += n_dot_l;
    }
    if (total <= 0.0) return {};
    return {static_cast<float>(sr / total), static_cast<float>(sg / total), static_cast<float>(sb / total)};
}

} // namespace saige::test::ibl
