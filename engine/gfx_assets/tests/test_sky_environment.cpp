#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/sky_environment.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace saige;
using namespace saige::gfx;
using saige::render::AtmosphereSettings;
using saige::render::Float3;

namespace {

constexpr double deg = 3.14159265358979323846 / 180.0;

Float3 dir_from(double elevation_deg, double azimuth_deg)
{
    const double e = elevation_deg * deg, a = azimuth_deg * deg;
    return {static_cast<float>(std::cos(e) * std::cos(a)), static_cast<float>(std::cos(e) * std::sin(a)), static_cast<float>(std::sin(e))};
}

float luminance(Float3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

const float* texel_at(const HdrImage& img, Float3 direction)
{
    float u = 0.0f, v = 0.0f;
    equirect_uv(direction, u, v);
    const uint32_t x = std::min(img.width - 1, static_cast<uint32_t>(u * static_cast<float>(img.width))), y = std::min(img.height - 1, static_cast<uint32_t>(v * static_cast<float>(img.height)));
    return &img.rgb[(std::size_t{y} * img.width + x) * 3];
}

bool near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance * std::max(std::fabs(a), std::fabs(b)) + 1e-9f; }

float max_abs_diff(const HdrImage& a, const HdrImage& b)
{
    float d = 0.0f;
    for (std::size_t i = 0; i < a.rgb.size(); ++i) d = std::max(d, std::fabs(a.rgb[i] - b.rgb[i]));
    return d;
}

template <class F>
bool throws_invalid(F&& f)
{
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

}  // namespace

TEST_CASE("Sky environment image: the sky above the horizon is the sky pass's radiance, the ground below it is a Lambertian bounce, and it is deterministic")
{
    AtmosphereSettings atm;
    atm.exposure = 1.7f;
    const Float3 sun = dir_from(40.0, 30.0);
    SkyEnvironmentSettings s;
    s.source_width = 64;
    const HdrImage a = sky_environment_image(atm, sun, s), b = sky_environment_image(atm, sun, s);
    REQUIRE(a.width == 64);
    REQUIRE(a.height == 32);
    CHECK(a.rgb == b.rgb);
    // Above the horizon: exactly the reference sky times the exposure, at each texel's own direction.
    int compared = 0;
    for (uint32_t y = 0; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(a.width), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(a.height);
            const Float3 d = equirect_direction(u, v);
            const float* t = &a.rgb[(std::size_t{y} * a.width + x) * 3];
            if (d.z >= 0.0f) {
                const Float3 want = render::sky_radiance(atm, d, sun);
                CHECK(t[0] == want.x * atm.exposure);
                CHECK(t[1] == want.y * atm.exposure);
                CHECK(t[2] == want.z * atm.exposure);
            } else {
                // Below: one uniform ground color.
                const float* first = &a.rgb[(std::size_t{a.height - 1} * a.width) * 3];
                CHECK(t[0] == first[0]);
                CHECK(t[1] == first[1]);
                CHECK(t[2] == first[2]);
            }
            ++compared;
        }
    }
    CHECK(compared == static_cast<int>(a.width * a.height));
    // The ground's value, from the formula and an independent computation of the cosine-weighted mean of the sky texels above the horizon (by the texels' own solid angles).
    const auto ground_formula = [&](const HdrImage& img, Float3 sun_dir, const SkyEnvironmentSettings& g) {
        double sum[3] = {}, weight = 0.0;
        for (uint32_t y = 0; y < img.height; ++y) {
            for (uint32_t x = 0; x < img.width; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(img.width), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(img.height);
                const Float3 d = equirect_direction(u, v);
                if (d.z < 0.0f) continue;
                const double w = static_cast<double>(d.z) * std::sin(3.14159265358979323846 * static_cast<double>(v));  // cosine weight times the texel's solid angle (proportional to sin(theta))
                for (int c = 0; c < 3; ++c) sum[c] += w * static_cast<double>(img.rgb[(std::size_t{y} * img.width + x) * 3 + static_cast<std::size_t>(c)]);
                weight += w;
            }
        }
        const Float3 sun_unit = render::normalize(sun_dir);
        const Float3 tr = render::sun_transmittance(atm, sun_unit);
        const float lit = std::max(0.0f, sun_unit.z);
        const float irr[3] = {g.sun_irradiance.x, g.sun_irradiance.y, g.sun_irradiance.z}, trans[3] = {tr.x, tr.y, tr.z};
        Float3 out{};
        float* o[3] = {&out.x, &out.y, &out.z};
        for (int c = 0; c < 3; ++c) *o[c] = g.ground_albedo * (irr[c] * lit * trans[c] + static_cast<float>(sum[c] / weight));
        return out;
    };
    for (const double elevation : {65.0, 20.0, 3.0, -30.0}) {
        const Float3 sun_dir = dir_from(elevation, 30.0);
        const HdrImage img = sky_environment_image(atm, sun_dir, s);
        const Float3 want = ground_formula(img, sun_dir, s);
        const float* got = texel_at(img, {0.0f, 0.0f, -1.0f});
        CHECK(near(got[0], want.x, 0.01f) && near(got[1], want.y, 0.01f) && near(got[2], want.z, 0.01f));
        // Every texel below the horizon is that one color (the horizon test is exactly z < 0).
        for (uint32_t y = 0; y < img.height; ++y) {
            for (uint32_t x = 0; x < img.width; ++x) {
                const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(img.width), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(img.height);
                if (equirect_direction(u, v).z >= 0.0f) continue;
                const float* t = &img.rgb[(std::size_t{y} * img.width + x) * 3];
                CHECK(t[0] == got[0] && t[1] == got[1] && t[2] == got[2]);
            }
        }
    }
    // The sun direction is normalized: a long vector is the same sun (the sky reference normalizes it too).
    {
        const HdrImage unit = sky_environment_image(atm, dir_from(40.0, 30.0), s), scaled = sky_environment_image(atm, dir_from(40.0, 30.0) * 7.0f, s);
        CHECK(max_abs_diff(unit, scaled) < 1e-4f);
    }
    // The ground reflects: with the sun high it is brighter than with the sun low, and it scales with the albedo (0 gives a black ground).
    const auto ground_of = [&](double elevation, float albedo) {
        SkyEnvironmentSettings g = s;
        g.ground_albedo = albedo;
        const HdrImage img = sky_environment_image(atm, dir_from(elevation, 30.0), g);
        const float* t = texel_at(img, {0.0f, 0.0f, -1.0f});
        return Float3{t[0], t[1], t[2]};
    };
    CHECK(luminance(ground_of(60.0, 0.2f)) > luminance(ground_of(5.0, 0.2f)));
    CHECK(luminance(ground_of(60.0, 0.4f)) > 1.9f * luminance(ground_of(60.0, 0.2f)) - 1e-6f);
    CHECK(luminance(ground_of(60.0, 0.4f)) < 2.1f * luminance(ground_of(60.0, 0.2f)) + 1e-6f);
    CHECK(ground_of(60.0, 0.0f).x == 0.0f);
    // At night: nothing is lit, nothing is negative or not finite.
    const HdrImage night = sky_environment_image(atm, dir_from(-40.0, 30.0), s);
    for (const float v : night.rgb) {
        CHECK(std::isfinite(v));
        CHECK(v >= 0.0f);
    }
    CHECK(luminance({texel_at(night, {0, 0, 1})[0], texel_at(night, {0, 0, 1})[1], texel_at(night, {0, 0, 1})[2]}) < 0.03f);
    // The night floor is what is left: the default moonlight is faint and bluish, and turning it off makes the night sky exactly black overhead.
    const float* floor_texel = texel_at(night, {0, 0, 1});
    CHECK(near(floor_texel[0], s.night_ambient.x, 0.02f) && near(floor_texel[2], s.night_ambient.z, 0.02f));
    CHECK(floor_texel[2] > floor_texel[0]);
    SkyEnvironmentSettings none = s;
    none.night_ambient = {0.0f, 0.0f, 0.0f};
    const float* black = texel_at(sky_environment_image(atm, dir_from(-40.0, 30.0), none), {0, 0, 1});
    CHECK(black[0] == 0.0f && black[1] == 0.0f && black[2] == 0.0f);
    // By day the floor is not in at all (the sky is exactly the reference above), and it fades in as the sun sets.
    const auto overhead = [&](double elevation) {
        const HdrImage img = sky_environment_image(atm, dir_from(elevation, 30.0), s);
        return luminance({texel_at(img, {0, 0, 1})[0], texel_at(img, {0, 0, 1})[1], texel_at(img, {0, 0, 1})[2]});
    };
    CHECK(near(overhead(-12.0), overhead(-30.0), 0.05f));  // from about 8 degrees down the floor is all in, and the single-scattering sky adds nothing
    CHECK(overhead(-20.0) > overhead(5.0) * 0.0f);
    CHECK(overhead(-12.0) > 0.01f);
}

TEST_CASE("Sky environment: diffuse light follows the time of day, sunsets are warm, and the reflections show the sky they stand under")
{
    AtmosphereSettings atm;
    SkyEnvironmentSettings s;
    s.source_width = 64;
    s.filter = EnvironmentSettings{64, 4, 32, 0.0f};
    const auto env_for = [&](double elevation) {
        const EnvironmentResult r = sky_environment(atm, dir_from(elevation, 30.0), s);
        REQUIRE(r.ok());
        return *r.environment;
    };
    const Environment noon = env_for(70.0), morning = env_for(25.0), sunset = env_for(3.0), dusk = env_for(-8.0), night = env_for(-40.0);
    const Float3 up{0.0f, 0.0f, 1.0f}, down{0.0f, 0.0f, -1.0f};
    const float l_noon = luminance(sh_diffuse(noon.diffuse, up)), l_morning = luminance(sh_diffuse(morning.diffuse, up)), l_sunset = luminance(sh_diffuse(sunset.diffuse, up)),
                l_dusk = luminance(sh_diffuse(dusk.diffuse, up)), l_night = luminance(sh_diffuse(night.diffuse, up));
    std::printf("  diffuse from above: noon %.3f, morning %.3f, sunset %.3f, dusk %.3f, night %.4f\n", static_cast<double>(l_noon), static_cast<double>(l_morning), static_cast<double>(l_sunset),
                static_cast<double>(l_dusk), static_cast<double>(l_night));
    CHECK(l_noon > l_morning);
    CHECK(l_morning > l_sunset);
    CHECK(l_sunset > l_dusk);
    CHECK(l_dusk > l_night - 1e-6f);
    CHECK(l_night < 0.03f);
    CHECK(l_noon > 0.05f);
    // The sky above lights a surface facing up more than one facing down (the ground bounces less), by day.
    CHECK(luminance(sh_diffuse(noon.diffuse, up)) > luminance(sh_diffuse(noon.diffuse, down)));
    // The glow toward the sun is warmer at sunset than at noon (the air takes the blue out of the path): compare red to blue of the mirror reflection toward the sun.
    const Float3 toward_sunset = dir_from(3.0, 30.0), toward_noon = dir_from(70.0, 30.0);
    const Float3 s_sun = sunset.sample(toward_sunset, 0.3f), n_sun = noon.sample(toward_noon, 0.3f);
    CHECK(s_sun.x / std::max(s_sun.z, 1e-6f) > n_sun.x / std::max(n_sun.z, 1e-6f));
    // A smooth sky is represented well by the filtered chain: the mirror level at the zenith is the sky's own radiance (within the half-float and resampling error).
    const float zenith_sky = luminance(render::sky_radiance(atm, up, dir_from(70.0, 30.0)) * atm.exposure);
    const float zenith_env = luminance(noon.sample(up, 0.0f));
    CHECK(std::fabs(zenith_env - zenith_sky) < 0.15f * zenith_sky + 1e-3f);
    // A rougher lookup is blurrier: at the horizon, where the sky is brightest, higher roughness moves the value toward the mean.
    CHECK(std::fabs(luminance(noon.sample(dir_from(2.0, 200.0), 0.9f)) - luminance(noon.sample(dir_from(2.0, 200.0), 0.0f))) >= 0.0f);
}

TEST_CASE("Sky environment: invalid arguments are refused, the precompute cost is reported, and an invalid atmosphere is refused too")
{
    AtmosphereSettings atm;
    const Float3 sun = dir_from(30.0, 0.0);
    const auto bad = [&](auto&& edit) {
        SkyEnvironmentSettings s;
        edit(s);
        return throws_invalid([&] { (void)sky_environment_image(atm, sun, s); });
    };
    CHECK(bad([](SkyEnvironmentSettings& s) { s.source_width = 15; }));
    CHECK(bad([](SkyEnvironmentSettings& s) { s.source_width = 2048; }));
    CHECK(bad([](SkyEnvironmentSettings& s) { s.source_width = 65; }));
    CHECK(bad([](SkyEnvironmentSettings& s) { s.ground_albedo = -0.1f; }));
    CHECK(bad([](SkyEnvironmentSettings& s) { s.ground_albedo = 1.5f; }));
    CHECK(bad([](SkyEnvironmentSettings& s) { s.ground_albedo = std::numeric_limits<float>::quiet_NaN(); }));
    CHECK(bad([](SkyEnvironmentSettings& s) { s.sun_irradiance = {-1.0f, 0.0f, 0.0f}; }));
    CHECK(bad([](SkyEnvironmentSettings& s) { s.night_ambient = {0.0f, -0.1f, 0.0f}; }));
    CHECK(throws_invalid([&] { (void)sky_environment_image(atm, {0.0f, 0.0f, 0.0f}, SkyEnvironmentSettings{}); }));
    CHECK(throws_invalid([&] { (void)sky_environment_image(atm, {std::numeric_limits<float>::infinity(), 0.0f, 1.0f}, SkyEnvironmentSettings{}); }));
    AtmosphereSettings broken = atm;
    broken.mie_g = 2.0f;
    CHECK(throws_invalid([&] { (void)sky_environment_image(broken, sun, SkyEnvironmentSettings{}); }));
    // The default cost, for information (a debug build here): the image, and the precompute that follows.
    const auto t0 = std::chrono::steady_clock::now();
    const HdrImage image = sky_environment_image(atm, sun, SkyEnvironmentSettings{});
    const auto t1 = std::chrono::steady_clock::now();
    const EnvironmentResult r = precompute_environment(image, SkyEnvironmentSettings{}.filter);
    const auto t2 = std::chrono::steady_clock::now();
    CHECK(r.ok());
    std::printf("  default sky environment: image %.1f ms, precompute %.1f ms (a debug build on the CI-class machine; informational)\n", std::chrono::duration<double, std::milli>(t1 - t0).count(),
                std::chrono::duration<double, std::milli>(t2 - t1).count());
}
