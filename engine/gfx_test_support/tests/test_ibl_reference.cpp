#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_test/ibl_reference.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace saige::test::ibl;
using saige::render::dot;
using saige::render::Float3;
using saige::render::normalize;

namespace {

bool near3(Float3 a, Float3 b, float tolerance)
{
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

/// The renderer's hemispherical ambient (pbr.hlsli / model.hlsl): ground color below, sky color above, linear in z.
Float3 gradient(Float3 d)
{
    const float t = d.z * 0.5f + 0.5f;
    const Float3 ground{0.20f, 0.18f, 0.16f}, sky{0.18f, 0.30f, 0.49f};
    return ground * (1.0f - t) + sky * t;
}

/// A smooth sky with a bright, wide sun lobe: the kind of content image-based lighting is for.
Float3 sky_with_sun(Float3 d)
{
    const Float3 sun = normalize(Float3{-0.4f, -0.55f, 0.73f});
    const float lobe = std::exp(6.0f * (dot(d, sun) - 1.0f));
    return gradient(d) + Float3{1.0f, 0.95f, 0.85f} * (8.0f * lobe);
}

} // namespace

TEST_CASE("IBL reference: a uniform environment reflects as itself under every operator")
{
    const auto uniform = [](Float3) { return Float3{0.7f, 0.4f, 0.2f}; };
    const Sh9 sh = sh_project(uniform);
    for (const Float3 n : {Float3{0, 0, 1}, Float3{0, 1, 0}, normalize(Float3{1, 1, -0.5f})}) {
        CHECK(near3(sh_diffuse(sh, n), {0.7f, 0.4f, 0.2f}, 2e-3f));
        CHECK(near3(diffuse_brute(uniform, n), {0.7f, 0.4f, 0.2f}, 2e-3f));
        for (const float roughness : {0.1f, 0.5f, 1.0f}) {
            CHECK(near3(ggx_prefilter(uniform, n, roughness), {0.7f, 0.4f, 0.2f}, 1e-3f));
        }
    }
}

TEST_CASE("IBL reference: a linear gradient has the closed-form irradiance a + (2/3) b n.z, which SH reproduces exactly")
{
    // L(d) = a + b d.z. The clamped-cosine convolution keeps the constant term and scales the linear (band 1) term by 2/3.
    const Float3 a{0.5f, 0.3f, 0.2f}, b{0.3f, 0.2f, -0.1f};
    const auto env = [&](Float3 d) { return a + b * d.z; };
    const Sh9 sh = sh_project(env);
    for (const float nz : {-1.0f, -0.4f, 0.0f, 0.6f, 1.0f}) {
        const Float3 n = normalize(Float3{std::sqrt(std::max(0.0f, 1.0f - nz * nz)), 0.0f, nz});
        const Float3 want = a + b * (2.0f / 3.0f * n.z);
        CHECK(near3(diffuse_brute(env, n), want, 2e-3f));
        CHECK(near3(sh_diffuse(sh, n), want, 2e-3f));
    }
}

TEST_CASE("IBL reference: the renderer's hemispherical ambient is not the exact irradiance of the gradient it models")
{
    // pbr.hlsli lights the diffuse term with L(n), the gradient evaluated at the normal. The exact diffuse radiance factor of that
    // same sky is a + (2/3) b n.z, so the shipped ambient exaggerates the sky-to-ground contrast by 3/2. Switching to image-based
    // lighting changes the look of every surface by this much; this test records it so the change is intended, not a surprise.
    const Sh9 sh = sh_project(gradient);
    double worst = 0.0;
    for (const float nz : {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f}) {
        const Float3 n{std::sqrt(std::max(0.0f, 1.0f - nz * nz)), 0.0f, nz};
        const Float3 shipped = gradient(n);
        const Float3 exact = sh_diffuse(sh, n);
        worst = std::max(worst, static_cast<double>(std::fabs(shipped.z - exact.z)));
        // The two agree where the gradient is flat (horizon normals) and differ most facing straight up or down.
        if (nz == 0.0f) CHECK(near3(shipped, exact, 3e-3f));
    }
    std::printf("  largest difference between L(n) and the exact diffuse radiance factor of the gradient: %.4f\n", worst);
    CHECK(worst > 0.02);   // 1/6 of the sky-ground difference in the blue channel: 0.0517
    CHECK(worst < 0.07);
}

namespace {

double worst_relative_sh_error(Float3 (*env)(Float3))
{
    const Sh9 sh = sh_project(env);
    double worst = 0.0;
    for (const Float3 n : {Float3{0, 0, 1}, Float3{0, 0, -1}, normalize(Float3{-0.4f, -0.55f, 0.73f}), normalize(Float3{0.4f, 0.55f, -0.73f}),
                           Float3{1, 0, 0}, normalize(Float3{0.3f, -0.8f, 0.2f})}) {
        const Float3 exact = diffuse_brute(env, n);
        const Float3 approx = sh_diffuse(sh, n);
        const double luminance = static_cast<double>(exact.x + exact.y + exact.z) / 3.0;
        worst = std::max(worst, std::fabs(static_cast<double>(approx.x + approx.y + approx.z) / 3.0 - luminance) / luminance);
    }
    return worst;
}

} // namespace

TEST_CASE("IBL reference: SH9 is accurate for a smooth sky and unreliable once a bright sun lobe is in the environment")
{
    const double sky_only = worst_relative_sh_error(gradient);
    const double with_sun = worst_relative_sh_error(sky_with_sun);
    std::printf("  SH9 diffuse worst relative error: smooth sky %.4f, sky plus an 8x sun lobe %.3f\n", sky_only, with_sun);
    CHECK(sky_only < 0.01);
    // A band-limited expansion cannot follow a concentrated light: facing away from it the exact irradiance is small and SH's ringing
    // is large in relative terms. The design consequence for M5: keep the sun out of the SH environment (it is a directional light
    // already) or use more than nine coefficients or a small irradiance cube.
    CHECK(with_sun > 0.05);
    CHECK(with_sun > 10.0 * sky_only);
    CHECK(with_sun < 0.30);
}

TEST_CASE("IBL reference: the GGX prefilter returns the environment at zero roughness and blurs a sun spike as roughness grows")
{
    const auto spike = [](Float3 d) {
        const Float3 sun = normalize(Float3{0.2f, 0.1f, 0.9f});
        return Float3{1.0f, 1.0f, 1.0f} * (0.1f + 50.0f * std::exp(400.0f * (dot(d, sun) - 1.0f)));
    };
    const Float3 toward_sun = normalize(Float3{0.2f, 0.1f, 0.9f});
    // Very low roughness (clamped to 0.045): essentially the environment itself in the mirror direction.
    CHECK(std::fabs(ggx_prefilter(spike, toward_sun, 0.0f, 16384).x - spike(toward_sun).x) < 0.1f * spike(toward_sun).x);
    // More roughness spreads the spike: the value toward the sun falls monotonically, and the contrast between the sun's direction and a
    // direction 23 degrees away shrinks as the lobe widens past that angle (the off-axis value itself is not monotone: it rises, then
    // falls as the filter averages over the whole hemisphere).
    const Float3 off_axis = normalize(Float3{0.5f, 0.1f, 0.7f});
    float previous_peak = 1e9f, previous_contrast = 1e9f;
    for (const float roughness : {0.1f, 0.2f, 0.4f, 0.7f, 1.0f}) {
        const float peak = ggx_prefilter(spike, toward_sun, roughness, 16384).x;
        const float off = ggx_prefilter(spike, off_axis, roughness, 16384).x;
        CHECK(peak < previous_peak);
        previous_peak = peak;
        CHECK(std::fabs(peak - off) < previous_contrast);
        previous_contrast = std::fabs(peak - off);
    }
    // Deterministic.
    CHECK(ggx_prefilter(spike, off_axis, 0.5f).x == ggx_prefilter(spike, off_axis, 0.5f).x);
}
