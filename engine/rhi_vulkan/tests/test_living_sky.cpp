// The recommended settings for a lit outdoor scene, run through the real renderer from noon to night: the atmosphere with a sun disc, stars and a moon, image-based lighting derived from that same sky,
// cascaded shadows and per-frame time-of-day lighting, HDR with the ACES fit. The test asserts what "good exposure" means in numbers (the day is neither blown out nor dark, the sky is brighter
// than the ground it lights, the night is dark but has stars, the sunset is warm and the noon is blue), and with SAIGE_CAPTURE_DIR set it also writes each frame as a binary PPM so the
// result can be looked at. Software Vulkan (lavapipe in CI); no physical-GPU claim.

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/gfx_assets/sky_environment.hpp"
#include "saige/render/atmosphere.hpp"
#include "saige/render/camera.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 320;
constexpr uint32_t kH = 180;
constexpr double deg = 3.14159265358979323846 / 180.0;

Float3 dir_from(double elevation_deg, double azimuth_deg)
{
    const double e = elevation_deg * deg, a = azimuth_deg * deg;
    return {static_cast<float>(std::cos(e) * std::cos(a)), static_cast<float>(std::cos(e) * std::sin(a)), static_cast<float>(std::sin(e))};
}

/// The settings this test (and docs/rendering/sky-environment.md) recommends for an outdoor scene.
RendererSettings recommended(const AtmosphereSettings& atmosphere, const std::shared_ptr<const EnvironmentLighting>& environment)
{
    RendererSettings s = deterministic_settings();
    s.hdr = true;
    s.tone_map.op = ToneMapOperator::AcesFitted;
    s.tone_map.exposure_ev = 0.0f;
    s.model_shading = ModelShading::MetallicRoughness;
    s.atmosphere = atmosphere;
    s.environment = environment;
    s.dynamic_lighting = true;
    s.shadows.enabled = true;
    s.shadows.map_size = 1024;
    s.shadows.cascades = 3;
    s.shadows.distance = 60.0f;
    s.fog_density = 0.006f;
    s.ground_half_size = 0.01f;  // the renderer's built-in checkerboard has fixed lighting (a 0.1 placeholder); the lit ground below is a model
    return s;
}

struct Frame {
    Image image;
    Float3 mean_sky, mean_ground;
    float saturated = 0.0f;  ///< fraction of pixels with every channel at 255
    float dark = 0.0f;       ///< fraction of pixels with every channel below 8
};

Frame render_at(double sun_elevation, const char* name, bool capture)
{
    AtmosphereSettings atm;
    atm.enabled = true;
    atm.exposure = 1.0f;
    atm.sun_intensity = 20.0f;
    const Float3 sun = dir_from(sun_elevation, 35.0);
    gfx::SkyEnvironmentSettings env_settings;
    env_settings.filter.width = 128;
    const gfx::EnvironmentResult env = gfx::sky_environment(atm, sun, env_settings);
    REQUIRE(env.ok());
    auto lighting = std::make_shared<EnvironmentLighting>(gfx::to_environment_lighting(*env.environment, 1.0f));

    const auto device = make_validated_device(name);
    RendererSettings settings = recommended(atm, lighting);
    SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, settings);
    OffscreenTarget target(*device, kW, kH);
    const auto material = [&](float r, float g, float b, float metallic, float roughness) {
        MaterialParams mp;
        mp.base_color[0] = r;
        mp.base_color[1] = g;
        mp.base_color[2] = b;
        mp.metallic = metallic;
        mp.roughness = roughness;
        return renderer.model_renderer().create_material(mp, {}, {}, "showcase");
    };
    const GpuModel rough = make_cube(renderer.registry(), material(0.8f, 0.2f, 0.15f, 0.0f, 0.8f));
    const GpuModel gold = make_cube(renderer.registry(), material(1.0f, 0.78f, 0.34f, 1.0f, 0.3f));
    const GpuModel mirror = make_cube(renderer.registry(), material(0.9f, 0.9f, 0.95f, 1.0f, 0.1f));
    const GpuModel white = make_cube(renderer.registry(), material(0.8f, 0.8f, 0.8f, 0.0f, 0.9f));
    const GpuModel floor_model = [&] {
        const Float3 n{0, 0, 1};
        const float half = 300.0f;
        const std::vector<ModelVertex> v = {{{-half, -half, 0}, n, {0, 0}}, {{half, -half, 0}, n, {1, 0}}, {{half, half, 0}, n, {1, 1}}, {{-half, half, 0}, n, {0, 1}}};
        GpuModel m;
        m.name = "ground";
        m.materials.push_back(material(0.45f, 0.42f, 0.36f, 0.0f, 0.95f));
        m.meshes.push_back({make_primitive(renderer.registry(), v, {0, 1, 2, 0, 2, 3}, 0)});
        m.placements.push_back({0, Mat4::identity()});
        return m;
    }();
    const ModelDraw draws[] = {{&floor_model, Mat4::identity()}, {&rough, translation({6.0f, 1.8f, 0.6f}) * scale_matrix({1.2f, 1.2f, 1.2f})},
                               {&gold, translation({7.0f, -0.6f, 0.7f}) * scale_matrix({1.4f, 1.4f, 1.4f})},
                               {&mirror, translation({8.5f, 1.2f, 0.9f}) * scale_matrix({1.8f, 1.8f, 1.8f})},
                               {&white, translation({5.0f, -2.2f, 0.5f}) * scale_matrix({1.0f, 1.0f, 1.0f})}};

    SceneLighting light = time_of_day_lighting(atm, sun, SceneLighting{});
    const Camera camera = look_at_camera({0.0f, 0.0f, 1.6f}, {10.0f, 0.5f, 2.4f}, {0.0f, 0.0f, 1.0f}, 70.0f * 3.14159265f / 180.0f);
    const Image img = target.render(renderer, {}, camera, 0, draws, nullptr, &light);
    CHECK(device->validation_error_count() == 0);

    Frame out;
    out.image = img;
    double sky[3] = {}, ground[3] = {};
    uint32_t sky_n = 0, ground_n = 0, saturated = 0, dark = 0;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const uint8_t* p = img.at(x, y);
            saturated += (p[0] == 255 && p[1] == 255 && p[2] == 255) ? 1 : 0;
            dark += (p[0] < 8 && p[1] < 8 && p[2] < 8) ? 1 : 0;
            if (y < kH / 5) {  // the top fifth of the image: sky (the camera looks a little above the horizon)
                for (int c = 0; c < 3; ++c) sky[c] += static_cast<double>(srgb_to_linear(p[c]));
                ++sky_n;
            } else if (y > kH * 4 / 5) {  // the bottom fifth: ground
                for (int c = 0; c < 3; ++c) ground[c] += static_cast<double>(srgb_to_linear(p[c]));
                ++ground_n;
            }
        }
    }
    out.mean_sky = {static_cast<float>(sky[0] / sky_n), static_cast<float>(sky[1] / sky_n), static_cast<float>(sky[2] / sky_n)};
    out.mean_ground = {static_cast<float>(ground[0] / ground_n), static_cast<float>(ground[1] / ground_n), static_cast<float>(ground[2] / ground_n)};
    out.saturated = static_cast<float>(saturated) / static_cast<float>(kW * kH);
    out.dark = static_cast<float>(dark) / static_cast<float>(kW * kH);
    if (capture) {
        if (const char* dir = std::getenv("SAIGE_CAPTURE_DIR"); dir != nullptr && dir[0] != '\0') {
            const std::string path = std::string(dir) + "/" + name + ".ppm";
            if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fprintf(f, "P6\n%u %u\n255\n", kW, kH);
                for (uint32_t i = 0; i < kW * kH; ++i) std::fwrite(&img.rgba[std::size_t{i} * 4], 1, 3, f);
                std::fclose(f);
            }
        }
    }
    return out;
}

float luminance(Float3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

}  // namespace

TEST_CASE("Living sky: the recommended outdoor settings give a well-exposed day, a warm sunset and a dark starry night")
{
    // The sun is in front of the camera (azimuth 35 degrees, the camera looks along +X, a little to +Y).
    const Frame noon = render_at(62.0, "noon", true);
    const Frame afternoon = render_at(28.0, "afternoon", true);
    const Frame sunset = render_at(4.0, "sunset", true);
    const Frame dusk = render_at(-7.0, "dusk", true);
    const Frame night = render_at(-35.0, "night", true);
    const auto report = [](const char* what, const Frame& f) {
        std::printf("  %-9s sky %.3f %.3f %.3f  ground %.3f %.3f %.3f  saturated %.2f%%  dark %.2f%%\n", what, static_cast<double>(f.mean_sky.x), static_cast<double>(f.mean_sky.y),
                    static_cast<double>(f.mean_sky.z), static_cast<double>(f.mean_ground.x), static_cast<double>(f.mean_ground.y), static_cast<double>(f.mean_ground.z),
                    static_cast<double>(f.saturated) * 100.0, static_cast<double>(f.dark) * 100.0);
    };
    report("noon", noon);
    report("afternoon", afternoon);
    report("sunset", sunset);
    report("dusk", dusk);
    report("night", night);
    // Daytime exposure: bright but not blown out, not dark.
    for (const Frame* f : {&noon, &afternoon}) {
        CHECK(luminance(f->mean_sky) > 0.08f);
        CHECK(luminance(f->mean_sky) < 0.85f);
        CHECK(f->saturated < 0.02f);  // under two percent of the pixels are pure white
        CHECK(f->dark < 0.10f);
        CHECK(luminance(f->mean_ground) > 0.03f);
        CHECK(luminance(f->mean_ground) < 0.7f);
    }
    // The sky is bluer than it is red at noon (Rayleigh), and the sunset sky is relatively redder than the noon sky.
    CHECK(noon.mean_sky.z > noon.mean_sky.x);
    CHECK(sunset.mean_sky.x / std::max(sunset.mean_sky.z, 1e-6f) > noon.mean_sky.x / std::max(noon.mean_sky.z, 1e-6f));
    // The brightness falls with the sun, through dusk to night.
    CHECK(luminance(noon.mean_sky) > luminance(sunset.mean_sky));
    CHECK(luminance(sunset.mean_sky) > luminance(dusk.mean_sky));
    CHECK(luminance(dusk.mean_sky) >= luminance(night.mean_sky));
    // Night: dark, not pure black everywhere (the stars, the moon's glow, the lit sides of the models), and nothing is blown.
    CHECK(luminance(night.mean_sky) < 0.01f);
    CHECK(luminance(night.mean_ground) < 0.02f);  // the lit ground goes dark with the sun
    CHECK(luminance(noon.mean_ground) > 4.0f * luminance(night.mean_ground));
    CHECK(night.dark > 0.5f);
    CHECK(night.saturated < 0.01f);
    // The frames are all different images.
    CHECK(noon.image.rgba != afternoon.image.rgba);
    CHECK(afternoon.image.rgba != sunset.image.rgba);
    CHECK(sunset.image.rgba != night.image.rgba);
}
