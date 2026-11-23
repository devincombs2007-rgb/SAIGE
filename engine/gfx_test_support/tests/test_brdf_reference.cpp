#include "saige/gfx_test/brdf_reference.hpp"
#include "saige/gfx_test/saige_test.hpp"

#include <cmath>
#include <vector>

using namespace saige::test::brdf;
using saige::render::Float3;

namespace {

constexpr Float3 up{0.0f, 0.0f, 1.0f};
constexpr double pi_d = 3.14159265358979323846;

Float3 direction(float theta, float phi)
{
    return {std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)};
}

/// Midpoint quadrature of g(theta, phi) over the upper hemisphere with the solid-angle element sin(theta).
template <typename G>
double integrate_hemisphere(G&& g, int theta_steps = 256, int phi_steps = 128)
{
    double sum = 0.0;
    const double dt = (pi_d / 2.0) / theta_steps;
    const double dp = (2.0 * pi_d) / phi_steps;
    for (int i = 0; i < theta_steps; ++i) {
        const double theta = (i + 0.5) * dt;
        for (int j = 0; j < phi_steps; ++j) {
            const double phi = (j + 0.5) * dp;
            sum += static_cast<double>(g(static_cast<float>(theta), static_cast<float>(phi))) * std::sin(theta) * dt * dp;
        }
    }
    return sum;
}

/// Directional albedo by quadrature over the light direction. Accurate for broad lobes and the diffuse term; it under-resolves
/// the narrow specular lobe of a smooth surface (use `albedo_by_half_vector` there).
double albedo_by_light(const Surface& s, float theta_v, int steps = 192)
{
    const Float3 v = direction(theta_v, 0.0f);
    return integrate_hemisphere(
        [&](float theta, float phi) {
            const Float3 l = direction(theta, phi);
            const Float3 f = evaluate(s, up, v, l);
            return (f.x + f.y + f.z) / 3.0f * std::cos(theta);
        },
        steps, steps);
}

/// Directional albedo of the *specular* lobe by importance-weighted quadrature over the half vector: the GGX distribution's CDF
/// is sampled uniformly, so the narrow lobe of a smooth surface is resolved at any roughness. For metallic = 1 this is the whole
/// BRDF; it does not resolve the (broad) diffuse term, so it is not used for dielectrics.
double albedo_by_half_vector(const Surface& s, float theta_v, int nu = 256, int nphi = 256)
{
    const Float3 v = direction(theta_v, 0.0f);
    const double alpha = alpha_from_roughness(s.roughness);
    double sum = 0.0;
    for (int i = 0; i < nu; ++i) {
        const double u = (i + 0.5) / nu;
        const double theta_h = std::atan(alpha * std::sqrt(u / (1.0 - u)));
        for (int j = 0; j < nphi; ++j) {
            const double phi_h = (j + 0.5) / nphi * 2.0 * pi_d;
            const Float3 h = direction(static_cast<float>(theta_h), static_cast<float>(phi_h));
            const float v_dot_h = dot(v, h);
            if (v_dot_h <= 0.0f) continue;
            const Float3 l = h * (2.0f * v_dot_h) - v;
            if (l.z <= 0.0f) continue;
            const Float3 f = evaluate(s, up, v, l);
            const double d = D_ggx(h.z, static_cast<float>(alpha));
            // dω_l = 4 (v.h) dω_h and dω_h = du dφ / (2π D cosθ_h) under the CDF sampling.
            const double weight = 4.0 * static_cast<double>(v_dot_h) / (2.0 * pi_d * d * static_cast<double>(h.z));
            const double mean_f = static_cast<double>(f.x + f.y + f.z) / 3.0;
            sum += mean_f * static_cast<double>(l.z) * weight / nu / nphi * 2.0 * pi_d;
        }
    }
    return sum;
}

} // namespace

TEST_CASE("BRDF reference: the GGX distribution is normalized over projected area")
{
    for (const float roughness : {0.3f, 0.5f, 0.8f, 1.0f}) {
        const float alpha = alpha_from_roughness(roughness);
        const double total = integrate_hemisphere([&](float theta, float) { return D_ggx(std::cos(theta), alpha) * std::cos(theta); },
                                                  2048, 8);
        CHECK(std::fabs(total - 1.0) < 0.01);
    }
}

TEST_CASE("BRDF reference: the visibility term has its known value at normal incidence and is symmetric")
{
    for (const float alpha : {0.05f, 0.3f, 1.0f}) {
        CHECK(std::fabs(V_smith_ggx_correlated(1.0f, 1.0f, alpha) - 0.25f) < 1e-6f);  // 0.5 / (1 + 1) for any alpha
        for (const float a : {0.1f, 0.5f, 0.9f}) {
            for (const float b : {0.2f, 0.6f, 1.0f}) {
                CHECK(std::fabs(V_smith_ggx_correlated(a, b, alpha) - V_smith_ggx_correlated(b, a, alpha)) < 1e-6f);
                CHECK(V_smith_ggx_correlated(a, b, alpha) > 0.0f);
            }
        }
    }
    CHECK(V_smith_ggx_correlated(0.0f, 0.0f, 0.5f) == 0.0f);  // no NaN at the horizon
}

TEST_CASE("BRDF reference: Schlick Fresnel runs from f0 at normal incidence to 1 at grazing, monotonically")
{
    const Float3 f0{0.04f, 0.5f, 1.0f};
    const Float3 at_normal = F_schlick(f0, 1.0f);
    const Float3 at_grazing = F_schlick(f0, 0.0f);
    CHECK(at_normal.x == 0.04f);
    CHECK(at_normal.y == 0.5f);
    CHECK(at_grazing.x == 1.0f);
    CHECK(at_grazing.y == 1.0f);
    float previous = 1.0f;
    for (int i = 0; i <= 20; ++i) {
        const float f = F_schlick(f0, static_cast<float>(i) / 20.0f).x;
        CHECK(f <= previous + 1e-6f);
        previous = f;
    }
}

TEST_CASE("BRDF reference: closed-form values at normal incidence and a mid-angle Fresnel")
{
    // Schlick at v.h = 0.5: f0 + (1 - f0) * 0.5^5 (the exponent is part of the specification).
    CHECK(std::fabs(F_schlick(Float3{0.04f, 0.04f, 0.04f}, 0.5f).x - (0.04f + 0.96f / 32.0f)) < 1e-6f);
    // View, light and normal aligned: h = n, so D = 1 / (pi alpha^2) and V = 0.25. A black dielectric reflects only
    // F0 * D * V = 0.04 * 0.25 / (pi alpha^2); a white metal reflects 1.0 * D * V.
    for (const float roughness : {0.3f, 0.6f, 1.0f}) {
        const float alpha = alpha_from_roughness(roughness);
        const float d_v = 0.25f / (pi * alpha * alpha);
        const Float3 black = evaluate(Surface{{0, 0, 0}, 0.0f, roughness}, up, up, up);
        CHECK(std::fabs(black.x - 0.04f * d_v) < 1e-5f * (1.0f + d_v));
        const Float3 metal = evaluate(Surface{{1, 1, 1}, 1.0f, roughness}, up, up, up);
        CHECK(std::fabs(metal.x - d_v) < 1e-5f * (1.0f + d_v));
        // The diffuse part of a white dielectric at normal incidence is (1 - 0.04) / pi plus the specular share.
        const Float3 white = evaluate(Surface{{1, 1, 1}, 0.0f, roughness}, up, up, up);
        CHECK(std::fabs(white.x - (0.96f / pi + 0.04f * d_v)) < 1e-5f * (1.0f + d_v));
        // metallic interpolates linearly between the two.
        const Float3 half = evaluate(Surface{{1, 1, 1}, 0.5f, roughness}, up, up, up);
        CHECK(std::fabs(half.x - 0.5f * (white.x + metal.x)) < 1e-5f * (1.0f + d_v));
    }
    // Below min_roughness the lobe stops getting narrower, so a perfect mirror gives a finite value.
    CHECK(alpha_from_roughness(0.0f) == alpha_from_roughness(min_roughness));
    CHECK(std::isfinite(evaluate(Surface{{1, 1, 1}, 1.0f, 0.0f}, up, up, up).x));
}

TEST_CASE("BRDF reference: the BRDF is reciprocal and zero below the horizon")
{
    for (const float metallic : {0.0f, 0.5f, 1.0f}) {
        for (const float roughness : {0.1f, 0.5f, 1.0f}) {
            const Surface s{{0.8f, 0.5f, 0.2f}, metallic, roughness};
            const Float3 v = direction(0.4f, 0.3f);
            const Float3 l = direction(1.0f, 2.2f);
            const Float3 a = evaluate(s, up, v, l);
            const Float3 b = evaluate(s, up, l, v);
            CHECK(std::fabs(a.x - b.x) < 1e-5f * (1.0f + a.x));
            CHECK(std::fabs(a.y - b.y) < 1e-5f * (1.0f + a.y));
            CHECK(std::fabs(a.z - b.z) < 1e-5f * (1.0f + a.z));
        }
    }
    const Surface s{};
    CHECK(evaluate(s, up, direction(0.5f, 0.0f), Float3{1.0f, 0.0f, -0.1f}).x == 0.0f);
    CHECK(evaluate(s, up, Float3{1.0f, 0.0f, -0.1f}, direction(0.5f, 0.0f)).x == 0.0f);
}

TEST_CASE("BRDF reference: the joint masking-shadowing term is bounded by each direction's own masking")
{
    // G = V * 4 n.l n.v must not exceed 1, nor min(G1(l), G1(v)) for the exact one-direction Smith terms.
    for (const float alpha : {0.0025f, 0.04f, 0.25f, 1.0f}) {
        const auto g1 = [&](float x) { return 2.0f * x / (x + std::sqrt(alpha * alpha + (1.0f - alpha * alpha) * x * x)); };
        for (float n_dot_l = 0.02f; n_dot_l <= 1.0f; n_dot_l += 0.049f) {
            for (float n_dot_v = 0.02f; n_dot_v <= 1.0f; n_dot_v += 0.049f) {
                const float g = V_smith_ggx_correlated(n_dot_l, n_dot_v, alpha) * 4.0f * n_dot_l * n_dot_v;
                CHECK(g <= 1.0f + 1e-5f);
                CHECK(g <= std::min(g1(n_dot_l), g1(n_dot_v)) + 1e-5f);
            }
        }
    }
}

TEST_CASE("BRDF reference: a metal never reflects more light than it receives")
{
    for (const float roughness : {0.1f, 0.3f, 0.6f, 1.0f}) {
        for (const float theta_v : {0.05f, 0.7f, 1.2f, 1.5f}) {
            const Surface s{{1.0f, 1.0f, 1.0f}, 1.0f, roughness};  // the brightest metal: f0 = 1
            CHECK(albedo_by_half_vector(s, theta_v) <= 1.0 + 0.01);
        }
    }
}

TEST_CASE("BRDF reference: a smooth white metal reflects almost all light, and a rough one loses the multiple-scattering part")
{
    const double smooth_albedo = albedo_by_half_vector(Surface{{1.0f, 1.0f, 1.0f}, 1.0f, 0.1f}, 0.3f);
    const double rough_albedo = albedo_by_half_vector(Surface{{1.0f, 1.0f, 1.0f}, 1.0f, 1.0f}, 0.3f);
    CHECK(smooth_albedo > 0.97);  // single scattering is nearly lossless at low roughness
    // At roughness 1 only microfacets within 45 degrees of the normal can reflect a normal-incidence ray into the upper
    // hemisphere, so at most half the light survives single scattering; the masking term takes some of that.
    CHECK(rough_albedo > 0.25);
    CHECK(rough_albedo < 0.5);
    double previous = 2.0;
    for (const float roughness : {0.1f, 0.3f, 0.5f, 0.7f, 1.0f}) {
        const double a = albedo_by_half_vector(Surface{{1.0f, 1.0f, 1.0f}, 1.0f, roughness}, 0.3f);
        CHECK(a <= previous + 0.005);  // the loss grows with roughness
        previous = a;
    }
}

TEST_CASE("BRDF reference: a rough white dielectric is near-Lambertian, and the glTF model overshoots 1 only at grazing views")
{
    // The specification's dielectric weights the diffuse term by (1 - F(v.h)), not by a function of n.v, so it is not exactly
    // energy conserving; the reference reproduces the specification, and this test records how far it strays.
    for (const float roughness : {0.6f, 1.0f}) {
        const Surface s{{1.0f, 1.0f, 1.0f}, 0.0f, roughness};
        for (const float theta_v : {0.05f, 0.7f}) {
            const double a = albedo_by_light(s, theta_v);
            CHECK(a > 0.95);
            CHECK(a < 1.02);
        }
        CHECK(albedo_by_light(s, 1.5f) < 1.15);  // observed up to about 1.08 at roughness 0.6
    }
    const Surface black{{0.0f, 0.0f, 0.0f}, 0.0f, 0.5f};  // specular only: small at normal incidence
    CHECK(albedo_by_light(black, 0.05f) < 0.08);
}

TEST_CASE("BRDF reference: directional shading applies the cosine and the light, and is deterministic")
{
    const Surface s{{0.5f, 0.5f, 0.5f}, 0.0f, 0.7f};
    const Float3 v = direction(0.3f, 0.0f);
    const Float3 l = direction(0.6f, 1.0f);
    const Float3 light{2.0f, 3.0f, 4.0f};
    const Float3 f = evaluate(s, up, v, l);
    const Float3 out = shade_directional(s, up, v, l, light);
    const float n_dot_l = std::cos(0.6f);
    CHECK(std::fabs(out.x - f.x * 2.0f * n_dot_l) < 1e-6f);
    CHECK(std::fabs(out.z - f.z * 4.0f * n_dot_l) < 1e-6f);
    CHECK(shade_directional(s, up, v, l, light).x == out.x);
    CHECK(shade_directional(s, up, v, Float3{0.0f, 0.0f, -1.0f}, light).x == 0.0f);
}

TEST_CASE("BRDF reference: the environment BRDF integral matches the BRDF it integrates and obeys energy bounds")
{
    // A metal with base color 1 reflects scale + bias, and with base color 0 reflects bias alone (F = weight). The independent
    // quadrature over the BRDF itself must agree with the specialised integral.
    for (const float roughness : {0.3f, 0.6f, 1.0f}) {
        for (const float theta_v : {0.2f, 0.8f, 1.3f}) {
            const double bias = albedo_by_half_vector(Surface{{0.0f, 0.0f, 0.0f}, 1.0f, roughness}, theta_v);
            const double total = albedo_by_half_vector(Surface{{1.0f, 1.0f, 1.0f}, 1.0f, roughness}, theta_v);
            const EnvBrdf ab = integrate_env_brdf(roughness, std::cos(theta_v));
            CHECK(std::fabs(total - bias - static_cast<double>(ab.scale)) < 0.01);
            CHECK(std::fabs(bias - static_cast<double>(ab.bias)) < 0.01);
            CHECK(ab.scale >= 0.0f);
            CHECK(ab.bias >= 0.0f);
            CHECK(ab.scale + ab.bias <= 1.0f + 1e-3f);  // a white f0 cannot reflect more than it receives
        }
    }
}

TEST_CASE("BRDF reference: ambient shading is zero in the dark, scales with occlusion, and a rough dielectric reflects its albedo")
{
    const Float3 v = direction(0.4f, 0.0f);
    const Surface dielectric{{0.6f, 0.4f, 0.2f}, 0.0f, 1.0f};
    const EnvBrdf ab = integrate_env_brdf(1.0f, std::cos(0.4f));
    CHECK(shade_ambient(dielectric, 1.0f, ab, {}, {}).x == 0.0f);
    const Float3 lit = shade_ambient(dielectric, 1.0f, ab, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f});
    const Float3 half = shade_ambient(dielectric, 0.5f, ab, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f});
    CHECK(std::fabs(half.y - 0.5f * lit.y) < 1e-6f);
    // Under a uniform white environment a rough dielectric reflects about its albedo plus a small specular term.
    CHECK(lit.x > 0.6f);
    CHECK(lit.x < 0.75f);
    (void)v;
}
