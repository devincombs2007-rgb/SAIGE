#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/environment.hpp"
#include "saige/render/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdint>
#include <functional>
#include <limits>

using namespace saige::gfx;
using saige::render::Float3;

namespace {

constexpr double pi_d = 3.14159265358979323846;

using Env = std::function<Float3(Float3)>;

HdrImage render_env(const Env& env, uint32_t w, uint32_t h)
{
    HdrImage im;
    im.width = w;
    im.height = h;
    im.rgb.resize(static_cast<std::size_t>(w) * h * 3);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const Float3 c = env(equirect_direction((static_cast<float>(x) + 0.5f) / static_cast<float>(w), (static_cast<float>(y) + 0.5f) / static_cast<float>(h)));
            float* p = im.rgb.data() + (static_cast<std::size_t>(y) * w + x) * 3;
            p[0] = c.x;
            p[1] = c.y;
            p[2] = c.z;
        }
    }
    return im;
}

/// Ground truth diffuse radiance factor: (1/pi) * the integral of L(l) max(n.l, 0) over the sphere, by quadrature of the analytic environment.
Float3 diffuse_truth(const Env& env, Float3 n, int steps = 400)
{
    double r = 0, g = 0, b = 0;
    const double dt = pi_d / steps, dp = 2.0 * pi_d / (2 * steps);
    for (int i = 0; i < steps; ++i) {
        const double theta = (i + 0.5) * dt, weight = std::sin(theta) * dt * dp;
        for (int j = 0; j < 2 * steps; ++j) {
            const double phi = (j + 0.5) * dp;
            const Float3 d{static_cast<float>(std::sin(theta) * std::cos(phi)), static_cast<float>(std::sin(theta) * std::sin(phi)), static_cast<float>(std::cos(theta))};
            const double cosine = std::max(0.0, static_cast<double>(saige::render::dot(n, d)));
            if (cosine == 0.0) continue;
            const Float3 l = env(d);
            r += static_cast<double>(l.x) * cosine * weight;
            g += static_cast<double>(l.y) * cosine * weight;
            b += static_cast<double>(l.z) * cosine * weight;
        }
    }
    return {static_cast<float>(r / pi_d), static_cast<float>(g / pi_d), static_cast<float>(b / pi_d)};
}

double radical_inverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return static_cast<double>(bits) * 2.3283064365386963e-10;
}

/// Ground truth split-sum prefilter of the analytic environment: the n.l-weighted mean over GGX importance samples (N = V = R), many samples.
Float3 prefilter_truth(const Env& env, Float3 r, double roughness, uint32_t samples = 20000)
{
    const double alpha = std::max(roughness, 0.045) * std::max(roughness, 0.045);
    const Float3 helper = std::fabs(r.z) < 0.999f ? Float3{0, 0, 1} : Float3{1, 0, 0};
    const Float3 tx = saige::render::normalize(saige::render::cross(helper, r));
    const Float3 ty = saige::render::cross(r, tx);
    double sr = 0, sg = 0, sb = 0, total = 0;
    for (uint32_t i = 0; i < samples; ++i) {
        const double u1 = (i + 0.5) / samples, u2 = radical_inverse(i), phi = 2 * pi_d * u2;
        const double ct = std::sqrt((1 - u1) / (1 + (alpha * alpha - 1) * u1)), st = std::sqrt(std::max(0.0, 1 - ct * ct));
        const Float3 h = tx * static_cast<float>(st * std::cos(phi)) + ty * static_cast<float>(st * std::sin(phi)) + r * static_cast<float>(ct);
        const Float3 l = h * (2.0f * saige::render::dot(r, h)) - r;
        const double ndl = saige::render::dot(r, l);
        if (ndl <= 0) continue;
        const Float3 c = env(l);
        sr += static_cast<double>(c.x) * ndl;
        sg += static_cast<double>(c.y) * ndl;
        sb += static_cast<double>(c.z) * ndl;
        total += ndl;
    }
    return {static_cast<float>(sr / total), static_cast<float>(sg / total), static_cast<float>(sb / total)};
}

bool near(Float3 a, Float3 b, float tol) { return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol && std::fabs(a.z - b.z) <= tol; }

const Float3 normals[] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {0, -1, 0}, {0.6f, 0.0f, 0.8f}, {-0.48f, 0.36f, -0.8f}, {0.5f, 0.5f, 0.70710678f}};

EnvironmentSettings small_settings()
{
    EnvironmentSettings s;
    s.width = 64;
    s.levels = 5;
    s.samples = 192;
    return s;
}

} // namespace

TEST_CASE("Environment: the equirectangular mapping puts +X in the middle, +Z at the top and +Y a quarter across")
{
    float u, v;
    equirect_uv({1, 0, 0}, u, v);
    CHECK(std::fabs(u - 0.5f) < 1e-6f && std::fabs(v - 0.5f) < 1e-6f);
    equirect_uv({0, 0, 1}, u, v);
    CHECK(std::fabs(v) < 1e-6f);
    equirect_uv({0, 0, -1}, u, v);
    CHECK(std::fabs(v - 1.0f) < 1e-6f);
    equirect_uv({0, 1, 0}, u, v);
    CHECK(std::fabs(u - 0.75f) < 1e-6f && std::fabs(v - 0.5f) < 1e-6f);
    equirect_uv({-1, 0, 0}, u, v);
    CHECK(std::fabs(u - 1.0f) < 1e-5f || std::fabs(u) < 1e-5f);  // the seam
    for (const float uu : {0.1f, 0.37f, 0.62f, 0.9f}) {
        for (const float vv : {0.1f, 0.5f, 0.83f}) {
            float ru, rv;
            equirect_uv(equirect_direction(uu, vv), ru, rv);
            CHECK(std::fabs(ru - uu) < 1e-5f && std::fabs(rv - vv) < 1e-5f);
        }
    }
}

TEST_CASE("Environment: a uniform environment gives its radiance everywhere, at every roughness and in the diffuse term")
{
    const Float3 c{0.7f, 0.4f, 0.2f};
    const EnvironmentResult r = precompute_environment(render_env([&](Float3) { return c; }, 128, 64), small_settings());
    REQUIRE(r.ok());
    const Environment& e = *r.environment;
    CHECK(e.version == kEnvironmentVersion && e.width == 64 && e.height == 32 && e.levels == 5);
    for (const Float3 n : normals) CHECK(near(sh_diffuse(e.diffuse, n), c, 2e-3f));
    for (const float rough : {0.0f, 0.3f, 0.5f, 1.0f}) {
        for (const Float3 d : normals) CHECK(near(e.sample(d, rough), c, 2e-3f));
    }
    // Layout: levels are halved, offsets add up, and the half-float upload has four channels per texel.
    CHECK(e.level_width(0) == 64 && e.level_height(0) == 32 && e.level_width(2) == 16 && e.level_height(4) == 2);
    std::size_t total = 0;
    for (uint32_t l = 0; l < 5; ++l) {
        CHECK(e.level_offset(l) == total);
        total += static_cast<std::size_t>(e.level_width(l)) * e.level_height(l) * 3;
    }
    CHECK(e.texels.size() == total);
    CHECK(e.to_rgba16f().size() == total / 3 * 4);
}

TEST_CASE("Environment: the diffuse term of a smooth sky matches the closed form and the quadrature ground truth")
{
    // L = a + b d.z: the exact diffuse radiance factor is a + (2/3) b n.z (the cosine lobe averages the linear term by 2/3).
    const Float3 a{0.6f, 0.6f, 0.7f}, b{0.4f, 0.3f, 0.2f};
    const Env sky = [&](Float3 d) { return Float3{a.x + b.x * d.z, a.y + b.y * d.z, a.z + b.z * d.z}; };
    const EnvironmentResult r = precompute_environment(render_env(sky, 256, 128), small_settings());
    REQUIRE(r.ok());
    for (const Float3 n : normals) {
        const Float3 closed{a.x + b.x * n.z * 2.0f / 3.0f, a.y + b.y * n.z * 2.0f / 3.0f, a.z + b.z * n.z * 2.0f / 3.0f};
        CHECK(near(sh_diffuse(r.environment->diffuse, n), closed, 3e-3f));
        CHECK(near(diffuse_truth(sky, n, 200), closed, 3e-3f));  // the oracle itself is right
    }
}

TEST_CASE("Environment: a bright sun lobe makes SH9 unreliable; the radiance limit removes it from the diffuse term, not the reflections")
{
    const Float3 sun_dir = saige::render::normalize(Float3{0.5f, 0.3f, 0.8f});
    const Env sky = [&](Float3 d) {
        const float s = std::max(0.0f, saige::render::dot(d, sun_dir));
        const float lobe = std::pow(s, 400.0f) * 400.0f;  // a small, very bright sun
        return Float3{0.4f + 0.2f * d.z + lobe, 0.5f + 0.2f * d.z + lobe, 0.7f + 0.2f * d.z + lobe};
    };
    const HdrImage image = render_env(sky, 256, 128);
    EnvironmentSettings plain = small_settings();
    EnvironmentSettings limited = small_settings();
    limited.diffuse_radiance_limit = 4.0f;
    const EnvironmentResult a = precompute_environment(image, plain), b = precompute_environment(image, limited);
    REQUIRE(a.ok());
    REQUIRE(b.ok());
    // Without the sun the environment is the smooth sky: the limited SH matches it, and the unlimited one is brighter toward the sun.
    const Env smooth = [&](Float3 d) { return Float3{0.4f + 0.2f * d.z, 0.5f + 0.2f * d.z, 0.7f + 0.2f * d.z}; };
    int closer = 0;
    for (const Float3 n : normals) {
        const Float3 truth = diffuse_truth(smooth, n, 150);
        const Float3 with_sun = sh_diffuse(a.environment->diffuse, n), without = sh_diffuse(b.environment->diffuse, n);
        closer += std::fabs(without.z - truth.z) < std::fabs(with_sun.z - truth.z) ? 1 : 0;
    }
    CHECK(closer >= 5);
    const float toward_sun = saige::render::dot(sun_dir, sun_dir);
    (void)toward_sun;
    CHECK(sh_diffuse(a.environment->diffuse, sun_dir).x > sh_diffuse(b.environment->diffuse, sun_dir).x + 0.05f);
    // The specular levels keep the sun at full strength in the mirror direction (the limit applies to the SH only).
    CHECK(a.environment->sample(sun_dir, 0.0f).x > 5.0f);
    CHECK(near(a.environment->sample(sun_dir, 0.0f), b.environment->sample(sun_dir, 0.0f), 1e-6f));
}

TEST_CASE("Environment: the prefiltered levels match an independent GGX importance-sampling ground truth")
{
    const Float3 sun_dir = saige::render::normalize(Float3{-0.3f, 0.4f, 0.6f});
    const Env env = [&](Float3 d) {
        const float s = std::max(0.0f, saige::render::dot(d, sun_dir));
        const float lobe = std::pow(s, 60.0f) * 6.0f;
        return Float3{0.3f + 0.2f * d.z + lobe, 0.4f + 0.1f * d.z + lobe, 0.6f + 0.1f * d.z + lobe};
    };
    // The coarsest equirect level is 16x8, which resolves the poles well enough to compare there (8x4 and below cannot: the nearest texel centers
    // are 45 degrees from the pole, so a documented minimum for the last level is 16 texels wide).
    EnvironmentSettings s;
    s.width = 128;
    s.levels = 4;
    s.samples = 256;
    const EnvironmentResult r = precompute_environment(render_env(env, 256, 128), s);
    REQUIRE(r.ok());
    int checked = 0;
    for (uint32_t level = 1; level < 4; ++level) {
        const double rough = static_cast<double>(level) / 3.0;
        for (const Float3 d : {sun_dir, Float3{0, 0, 1}, Float3{0.8f, 0.0f, 0.6f}, Float3{0.0f, -1.0f, 0.0f}, saige::render::normalize(Float3{0.2f, 0.7f, 0.4f})}) {
            const Float3 truth = prefilter_truth(env, d, rough);
            const Float3 got = r.environment->sample(d, static_cast<float>(rough));
            const float tol = 0.03f + 0.08f * truth.x;  // importance-sampling noise and the equirect resolution
            if (!near(got, truth, tol)) std::printf("  level %u dir (%.2f %.2f %.2f): got %.3f truth %.3f\n", level, static_cast<double>(d.x), static_cast<double>(d.y), static_cast<double>(d.z), static_cast<double>(got.x), static_cast<double>(truth.x));
            CHECK(near(got, truth, tol));
            ++checked;
        }
    }
    CHECK(checked == 15);
    // Rougher is blurrier: the peak toward the sun falls monotonically with roughness, and the mirror level keeps the environment.
    float previous = r.environment->sample(sun_dir, 0.0f).x;
    CHECK(near(r.environment->sample(sun_dir, 0.0f), env(sun_dir), 0.35f * env(sun_dir).x));
    for (const float rough : {0.34f, 0.67f, 1.0f}) {
        const float v = r.environment->sample(sun_dir, rough).x;
        CHECK(v < previous);
        previous = v;
    }
}

TEST_CASE("Environment: precomputation is deterministic and validates its input")
{
    const Env env = [](Float3 d) { return Float3{0.5f + 0.4f * d.x, 0.5f + 0.4f * d.y, 0.5f + 0.4f * d.z}; };
    const HdrImage image = render_env(env, 64, 32);
    const EnvironmentResult a = precompute_environment(image, small_settings()), b = precompute_environment(image, small_settings());
    REQUIRE(a.ok() && b.ok());
    CHECK(a.environment->texels == b.environment->texels);
    CHECK(std::memcmp(&a.environment->diffuse, &b.environment->diffuse, sizeof(Sh9)) == 0);
    const auto rejected = [&](const HdrImage& im, const EnvironmentSettings& s, const char* what) {
        const EnvironmentResult r = precompute_environment(im, s);
        const bool ok = !r.ok() && r.error.find(what) != std::string::npos;
        if (!ok) std::printf("  expected '%s', got '%s'\n", what, r.error.c_str());
        return ok;
    };
    EnvironmentSettings s = small_settings();
    s.width = 7;
    CHECK(rejected(image, s, "even"));
    s = small_settings();
    s.levels = 0;
    CHECK(rejected(image, s, "level count"));
    s = small_settings();
    s.samples = 0;
    CHECK(rejected(image, s, "sample count"));
    s = small_settings();
    s.diffuse_radiance_limit = -1.0f;
    CHECK(rejected(image, s, "radiance limit"));
    HdrImage bad = image;
    bad.rgb[5] = std::numeric_limits<float>::quiet_NaN();
    CHECK(rejected(bad, small_settings(), "non-finite"));
    bad = image;
    bad.rgb[7] = -0.5f;
    CHECK(rejected(bad, small_settings(), "negative"));
    bad = image;
    bad.rgb.pop_back();
    CHECK(rejected(bad, small_settings(), "does not match"));
    bad = image;
    bad.width = 1;
    CHECK(rejected(bad, small_settings(), "at least 2x1"));
    // A single level is just the mirror; a source of another size is resampled.
    s = small_settings();
    s.levels = 1;
    const EnvironmentResult one = precompute_environment(render_env(env, 200, 100), s);
    REQUIRE(one.ok());
    CHECK(one.environment->texels.size() == 64 * 32 * 3);
    CHECK(near(one.environment->sample({1, 0, 0}, 1.0f), env({1, 0, 0}), 0.03f));
}

TEST_CASE("Environment: half-float conversion rounds to nearest even and saturates")
{
    CHECK(float_to_half_saturate(0.0f) == 0x0000);
    CHECK(float_to_half_saturate(-0.0f) == 0x8000);
    CHECK(float_to_half_saturate(1.0f) == 0x3C00);
    CHECK(float_to_half_saturate(-2.0f) == 0xC000);
    CHECK(float_to_half_saturate(0.5f) == 0x3800);
    CHECK(float_to_half_saturate(65504.0f) == 0x7BFF);
    CHECK(float_to_half_saturate(1.0e6f) == 0x7BFF);                                   // saturates instead of becoming infinity
    CHECK(float_to_half_saturate(std::numeric_limits<float>::infinity()) == 0x7BFF);
    CHECK(float_to_half_saturate(std::numeric_limits<float>::quiet_NaN()) == 0x0000);
    CHECK(float_to_half_saturate(std::ldexp(1.0f, -24)) == 0x0001);                    // the smallest subnormal
    CHECK(float_to_half_saturate(std::ldexp(1.0f, -14)) == 0x0400);                    // the smallest normal
    CHECK(float_to_half_saturate(1e-9f) == 0x0000);
    CHECK(float_to_half_saturate(1.0f + std::ldexp(1.0f, -11)) == 0x3C00);             // a tie rounds to the even mantissa (down)
    CHECK(float_to_half_saturate(1.0f + 3.0f * std::ldexp(1.0f, -11)) == 0x3C02);      // a tie rounds to the even mantissa (up)
    CHECK(float_to_half_saturate(1.0f + std::ldexp(1.0f, -11) + std::ldexp(1.0f, -20)) == 0x3C01);  // just above a tie rounds up
    CHECK(float_to_half_saturate(65519.0f) == 0x7BFF);
    // Every half value that is exactly representable converts back to itself.
    int checked = 0;
    for (uint32_t h = 0; h < 0x7C00; h += 7) {
        const uint32_t sign = 0, exp = (h >> 10) & 31, man = h & 1023;
        const float f = exp == 0 ? std::ldexp(static_cast<float>(man), -24) : std::ldexp(1.0f + static_cast<float>(man) / 1024.0f, static_cast<int>(exp) - 15);
        CHECK(float_to_half_saturate(f) == static_cast<uint16_t>(sign | h));
        ++checked;
    }
    CHECK(checked > 4000);
}
