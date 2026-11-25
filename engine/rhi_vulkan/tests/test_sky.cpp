// The atmosphere's sky pass on a real (validated) Vulkan device, against the CPU reference (saige::render::sky_radiance). Each probe pixel's view direction is
// rebuilt from the camera and compared with 1 - exp(-radiance * exposure), the curve an LDR scene gets. Also: the sun's side of the sky is redder at sunset,
// the fog fades distant ground into the horizon color, and a scene without the atmosphere keeps its flat clear color.

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/render/atmosphere.hpp"
#include "saige/render/camera.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 160;
constexpr uint32_t kH = 120;
constexpr double deg = 3.14159265358979323846 / 180.0;

Float3 dir_from(double elevation_deg, double azimuth_deg)
{
    const double e = elevation_deg * deg, a = azimuth_deg * deg;
    return {static_cast<float>(std::cos(e) * std::cos(a)), static_cast<float>(std::cos(e) * std::sin(a)), static_cast<float>(std::sin(e))};
}

RendererSettings sky_settings(Float3 sun, float exposure)
{
    RendererSettings s = deterministic_settings();
    s.lighting.sun_direction = sun;
    s.atmosphere.enabled = true;
    s.atmosphere.exposure = exposure;
    s.fog_density = 0.0f;
    s.ground_half_size = 0.01f;  // the checkerboard ground would cover everything below the horizon; these tests are about the sky
    return s;
}

/// The view direction through pixel (x, y) of an image rendered with `camera` (the pixel center).
Float3 pixel_direction(const Camera& camera, int x, int y)
{
    const float aspect = static_cast<float>(kW) / static_cast<float>(kH);
    const float tan_half = std::tan(camera.fov_y_radians * 0.5f);
    const float ndc_x = (static_cast<float>(x) + 0.5f) / static_cast<float>(kW) * 2.0f - 1.0f;
    const float ndc_y = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(kH) * 2.0f;
    return normalize(camera_forward(camera) + camera_right(camera) * (tan_half * aspect * ndc_x) + camera_up(camera) * (tan_half * ndc_y));
}

Float3 ldr(Float3 radiance, float exposure) { return {1.0f - std::exp(-radiance.x * exposure), 1.0f - std::exp(-radiance.y * exposure), 1.0f - std::exp(-radiance.z * exposure)}; }

Float3 linear_at(const Image& img, int x, int y)
{
    const uint8_t* c = img.at(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    return {srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2])};
}

Image render_sky(const RendererSettings& settings, const Camera& camera, const char* name)
{
    const auto device = make_validated_device(name);
    SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, settings);
    OffscreenTarget target(*device, kW, kH);
    const Image img = target.render(renderer, {}, camera, 0, {});
    CHECK(device->validation_error_count() == 0);
    return img;
}

} // namespace

TEST_CASE("Sky: the pass matches the CPU reference at the sun's elevations, the camera's pitches and over the whole image")
{
    int probes = 0;
    float worst = 0.0f;
    for (const double sun_elevation : {60.0, 20.0, 3.0, -3.0, -9.0}) {
        const Float3 sun = dir_from(sun_elevation, 20.0);
        const float exposure = sun_elevation > 10.0 ? 1.5f : (sun_elevation > 0.0 ? 3.0f : 40.0f);  // twilight is dim: expose it so the curve is not near zero
        for (const double pitch : {80.0, 30.0, 0.0, -10.0}) {
            // The camera looks toward azimuth 20 degrees (toward the sun's side) with the given pitch above the horizon.
            const Float3 forward = dir_from(pitch, 20.0);
            const Camera camera = look_at_camera({0.0f, 0.0f, 1.0f}, Float3{0, 0, 1} + forward * 5.0f);
            const Image img = render_sky(sky_settings(sun, exposure), camera, "sky_parity");
            for (int y = 4; y < static_cast<int>(kH); y += 28) {
                for (int x = 4; x < static_cast<int>(kW); x += 38) {
                    const Float3 d = pixel_direction(camera, x, y);
                    const Float3 want = ldr(sky_radiance(sky_settings(sun, exposure).atmosphere, d, normalize(sun)), exposure);
                    const Float3 got = linear_at(img, x, y);
                    const float err = std::max({std::fabs(got.x - want.x), std::fabs(got.y - want.y), std::fabs(got.z - want.z)});
                    worst = std::max(worst, err);
                    if (err > 0.02f) {
                        std::printf("  sun %.0f pitch %.0f pixel (%d, %d): got (%.3f %.3f %.3f) want (%.3f %.3f %.3f)\n", sun_elevation, pitch, x, y, static_cast<double>(got.x),
                                    static_cast<double>(got.y), static_cast<double>(got.z), static_cast<double>(want.x), static_cast<double>(want.y), static_cast<double>(want.z));
                    }
                    CHECK(err <= 0.02f);
                    ++probes;
                }
            }
        }
    }
    std::printf("  %d probes, worst channel error %.4f\n", probes, static_cast<double>(worst));
    CHECK(probes >= 20 * 24);
}

TEST_CASE("Sky: at sunset the sun's side is redder than the opposite side, and an exposure of zero is black")
{
    const Float3 sun = dir_from(3.0, 90.0);
    const Camera toward = look_at_camera({0, 0, 1}, Float3{0, 0, 1} + dir_from(10.0, 90.0) * 5.0f), away = look_at_camera({0, 0, 1}, Float3{0, 0, 1} + dir_from(10.0, 270.0) * 5.0f);
    const RendererSettings s = sky_settings(sun, 0.25f);  // a low exposure keeps the curve out of saturation, where colors converge to white
    const Image a = render_sky(s, toward, "sky_sunset_toward"), b = render_sky(s, away, "sky_sunset_away");
    const Float3 near_sun = linear_at(a, static_cast<int>(kW) / 2, static_cast<int>(kH) / 2), far_side = linear_at(b, static_cast<int>(kW) / 2, static_cast<int>(kH) / 2);
    std::printf("  toward the sun (%.3f %.3f %.3f), away (%.3f %.3f %.3f)\n", static_cast<double>(near_sun.x), static_cast<double>(near_sun.y), static_cast<double>(near_sun.z),
                static_cast<double>(far_side.x), static_cast<double>(far_side.y), static_cast<double>(far_side.z));
    CHECK(near_sun.x / std::max(near_sun.z, 1e-4f) > 1.4f * (far_side.x / std::max(far_side.z, 1e-4f)));  // redder toward the sun
    CHECK(near_sun.x > far_side.x);                                                                       // and brighter
    RendererSettings dark = sky_settings(sun, 0.0f);
    const Image black = render_sky(dark, toward, "sky_black");
    const Float3 c = linear_at(black, 10, 10);
    CHECK(c.x == 0.0f && c.y == 0.0f && c.z == 0.0f);
}

TEST_CASE("Sky: distant ground fades into the horizon color, and a scene without the atmosphere keeps its clear color")
{
    const Float3 sun = dir_from(40.0, 0.0);
    RendererSettings s = sky_settings(sun, 1.5f);
    s.fog_density = 0.5f;  // by 40 m everything is fog
    s.ground_half_size = 400.0f;  // (the ground this test is about)
    const Camera camera = look_at_camera({0.0f, 0.0f, 1.5f}, {40.0f, 0.0f, 1.0f});
    const auto device = make_validated_device("sky_fog");
    SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, s);
    OffscreenTarget target(*device, kW, kH);
    const Image img = target.render(renderer, {}, camera, 0, {});
    CHECK(device->validation_error_count() == 0);
    const Float3 horizon_radiance = sky_horizon_color(s.atmosphere, normalize(sun));  // already times the exposure
    const Float3 expected = {1.0f - std::exp(-horizon_radiance.x), 1.0f - std::exp(-horizon_radiance.y), 1.0f - std::exp(-horizon_radiance.z)};
    // A ground pixel just below the horizon line (the far ground, fully fogged).
    int row = -1;
    for (int y = static_cast<int>(kH) / 2; y < static_cast<int>(kH); ++y) {
        const Float3 d = pixel_direction(camera, static_cast<int>(kW) / 2, y);
        if (d.z < -0.02f) {  // a little below the horizon: the ground at tens of meters
            row = y;
            break;
        }
    }
    REQUIRE(row > 0);
    const Float3 ground = linear_at(img, static_cast<int>(kW) / 2, row);
    std::printf("  fogged ground (%.3f %.3f %.3f), horizon color (%.3f %.3f %.3f)\n", static_cast<double>(ground.x), static_cast<double>(ground.y), static_cast<double>(ground.z),
                static_cast<double>(expected.x), static_cast<double>(expected.y), static_cast<double>(expected.z));
    CHECK(near3(ground, expected, 0.03f));
    // Without the atmosphere nothing changes: the flat clear color is what an empty scene shows.
    RendererSettings flat = deterministic_settings();
    flat.fog_density = 0.0f;
    const auto device2 = make_validated_device("sky_off");
    SceneRenderer plain(*device2, rhi::Format::RGBA8_SRGB, flat);
    OffscreenTarget target2(*device2, kW, kH);
    const Image empty = target2.render(plain, {}, look_at_camera({0, 0, 1.5f}, {10, 0, 3.0f}), 0, {});
    const Float3 sky_color = linear_at(empty, static_cast<int>(kW) / 2, 5);
    CHECK(near3(sky_color, {flat.sky.r, flat.sky.g, flat.sky.b}, 0.01f));
    CHECK(device2->validation_error_count() == 0);
}

TEST_CASE("Sky: a sun direction of any finite magnitude gives the sky, the fog and the material of the unit direction")
{
    // validate_lighting accepts every finite non-zero direction; a plain float normalize squares the components, so a huge or tiny direction used to become a zero vector for the
    // sky and the fog while the model lighting (which normalizes safely) still pointed the right way.
    const Float3 unit = dir_from(35.0, 70.0);
    const auto render = [&](Float3 sun_direction, const char* name) {
        RendererSettings s = sky_settings(sun_direction, 1.5f);
        s.fog_density = 0.04f;
        s.ground_half_size = 200.0f;
        const auto device = make_validated_device(name);
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, s);
        OffscreenTarget target(*device, kW, kH);
        MaterialParams grey;
        grey.base_color[0] = grey.base_color[1] = grey.base_color[2] = 0.7f;
        const GpuModel cube = make_cube(renderer.registry(), renderer.model_renderer().create_material(grey, {}, {}, "cube"));
        const ModelDraw draws[] = {{&cube, translation({6.0f, 0.0f, 0.5f})}};
        const Camera camera = look_at_camera({0.0f, 0.0f, 1.5f}, {10.0f, 0.0f, 3.0f});
        const Image img = target.render(renderer, {}, camera, 0, draws);
        CHECK(device->validation_error_count() == 0);
        return img;
    };
    const Image reference = render(unit, "sky_scale_unit");
    for (const float scale : {3.0e38f, 1.0e30f, 1.0e19f, 1.0e-19f, 1.0e-30f, 1.0e-37f}) {
        const Image scaled = render(unit * scale, "sky_scale");
        int worst = 0;
        for (std::size_t i = 0; i < reference.rgba.size(); ++i) worst = std::max(worst, std::abs(int{reference.rgba[i]} - int{scaled.rgba[i]}));
        std::printf("  scale %g: worst 8-bit channel delta %d\n", static_cast<double>(scale), worst);
        CHECK(worst <= 1);  // sky, horizon fog on the ground and the lit cube all follow the same direction
    }
}

// ---- Celestial bodies on the GPU ---------------------------------------------------------------------------------------------------------------

namespace {

constexpr float kNarrowFov = 4.0f * 3.14159265f / 180.0f;
constexpr float kMidFov = 40.0f * 3.14159265f / 180.0f;

struct BodyCompare {
    int compared = 0, bad = 0, lit = 0;
    float worst = 0.0f;
};

/// Renders the sky pass for `settings` toward `camera` and compares every pixel with `ldr(sky_with_bodies(...))`.
BodyCompare compare_bodies(const RendererSettings& settings, const Camera& camera, Float3 sun, float tolerance, const char* name)
{
    const Image img = render_sky(settings, camera, name);
    BodyCompare out;
    for (int y = 0; y < static_cast<int>(kH); ++y) {
        for (int x = 0; x < static_cast<int>(kW); ++x) {
            const Float3 d = pixel_direction(camera, x, y);
            const Float3 want = ldr(sky_with_bodies(settings.atmosphere, d, normalize(sun)), settings.atmosphere.exposure);
            const Float3 got = linear_at(img, x, y);
            const float err = std::max({std::fabs(got.x - want.x), std::fabs(got.y - want.y), std::fabs(got.z - want.z)});
            ++out.compared;
            out.worst = std::max(out.worst, err);
            const Float3 plain = ldr(sky_radiance(settings.atmosphere, d, normalize(sun)), settings.atmosphere.exposure);
            if (std::fabs(want.x - plain.x) + std::fabs(want.y - plain.y) + std::fabs(want.z - plain.z) > 0.05f) ++out.lit;  // pixels where a body is visible
            if (err > tolerance) {
                if (++out.bad <= 5) std::printf("  pixel (%d, %d): got (%.3f %.3f %.3f) want (%.3f %.3f %.3f)\n", x, y, static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z),
                                                static_cast<double>(want.x), static_cast<double>(want.y), static_cast<double>(want.z));
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("Sky bodies: the sun's disc and the moon match the CPU reference pixel by pixel, and the disc sits where the sun is")
{
    // The sun's disc: a narrow view centered a little off the sun, so the disc, its soft edge and the sky around it are all in the image.
    {
        const Float3 sun = dir_from(30.0, 40.0);
        RendererSettings s = sky_settings(sun, 1.0f);
        s.atmosphere.sun_disc_intensity = 2.0f;
        const Float3 look = dir_from(30.0 + 0.4, 40.0 + 0.5);
        const Camera camera = look_at_camera({0.0f, 0.0f, 1.0f}, Float3{0, 0, 1} + look * 5.0f, {0.0f, 0.0f, 1.0f}, kNarrowFov);
        const BodyCompare c = compare_bodies(s, camera, sun, 0.04f, "sky_disc");
        std::printf("  sun disc: %d pixels, %d show the disc, %d off by more than 0.04 (worst %.3f)\n", c.compared, c.lit, c.bad, static_cast<double>(c.worst));
        CHECK(c.lit >= 150);                 // the disc is about 16 pixels across at this field of view
        CHECK(c.bad <= 6);                   // at most a few pixels on the soft edge differ by rounding
        CHECK(c.worst <= 0.3f);
        // Without the disc the same view is just sky: the disc is what the intensity adds.
        RendererSettings none = s;
        none.atmosphere.sun_disc_intensity = 0.0f;
        const Image with = render_sky(s, camera, "sky_disc_on"), without = render_sky(none, camera, "sky_disc_off");
        int differing = 0;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) differing += max_channel_delta(with.at(x, y), without.at(x, y)) > 8 ? 1 : 0;
        }
        CHECK(differing >= 150);
    }
    // The moon: the sun is below the horizon, the moon opposite it, above.
    {
        const Float3 sun = dir_from(-20.0, 40.0);
        RendererSettings s = sky_settings(sun, 1.0f);
        s.atmosphere.moon_intensity = 1.5f;
        const Float3 moon = dir_from(20.0, 220.0);
        const Float3 look = dir_from(20.0 + 0.3, 220.0 - 0.4);
        const Camera camera = look_at_camera({0.0f, 0.0f, 1.0f}, Float3{0, 0, 1} + look * 5.0f, {0.0f, 0.0f, 1.0f}, kNarrowFov);
        (void)moon;
        const BodyCompare c = compare_bodies(s, camera, sun, 0.04f, "sky_moon");
        std::printf("  moon: %d pixels, %d show it, %d off by more than 0.04 (worst %.3f)\n", c.compared, c.lit, c.bad, static_cast<double>(c.worst));
        CHECK(c.lit >= 150);
        CHECK(c.bad <= 6);
    }
}

TEST_CASE("Sky bodies: stars match the CPU field pixel by pixel at night, and the sky is empty of them in daylight")
{
    const Float3 night_sun = dir_from(-40.0, 0.0);
    RendererSettings s = sky_settings(night_sun, 1.0f);
    s.atmosphere.star_intensity = 3.0f;
    const Float3 look = dir_from(65.0, 100.0);
    const Camera camera = look_at_camera({0.0f, 0.0f, 1.0f}, Float3{0, 0, 1} + look * 5.0f, {0.0f, 0.0f, 1.0f}, kMidFov);
    const BodyCompare c = compare_bodies(s, camera, night_sun, 0.05f, "sky_stars");
    std::printf("  stars: %d pixels, %d show a star, %d off by more than 0.05 (worst %.3f)\n", c.compared, c.lit, c.bad, static_cast<double>(c.worst));
    CHECK(c.lit >= 25);
    CHECK(c.bad <= c.lit / 10 + 1);  // a star whose cell boundary is rounded the other way on the GPU is the only legitimate difference
    // Toward the horizon the stars fade out and stop at it: a view straddling the horizon (the sky is below it too, where the planet is, and nothing there is a star).
    const Float3 low_look = dir_from(8.0, 100.0);
    const Camera low_camera = look_at_camera({0.0f, 0.0f, 1.0f}, Float3{0, 0, 1} + low_look * 5.0f, {0.0f, 0.0f, 1.0f}, kMidFov);
    RendererSettings dense = s;
    dense.atmosphere.star_intensity = 40.0f;  // bright enough that a star the horizon fade wrongly lets through would show
    const BodyCompare low = compare_bodies(dense, low_camera, night_sun, 0.05f, "sky_stars_horizon");
    std::printf("  stars at the horizon: %d show a star, %d off by more than 0.05 (worst %.3f)\n", low.lit, low.bad, static_cast<double>(low.worst));
    CHECK(low.lit >= 5);
    CHECK(low.bad <= low.lit / 10 + 1);
    // Daylight: the same view with the sun up has no stars.
    const Float3 day_sun = dir_from(40.0, 0.0);
    RendererSettings day = sky_settings(day_sun, 1.0f);
    day.atmosphere.star_intensity = 3.0f;
    RendererSettings off = day;
    off.atmosphere.star_intensity = 0.0f;
    const Image a = render_sky(day, camera, "sky_stars_day"), b = render_sky(off, camera, "sky_stars_off");
    CHECK(a.rgba == b.rgba);
}
