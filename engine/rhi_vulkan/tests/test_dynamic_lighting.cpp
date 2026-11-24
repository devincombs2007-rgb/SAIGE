// Per-frame lighting (RendererSettings::dynamic_lighting) on a real (validated) Vulkan device. A sequence of frames, more than there are frames in flight, each with its own
// sun: the ground's pixels follow an independent CPU evaluation of the lighting, the shadow of a column falls where an independent ray cast says for that frame's sun, the sky
// pass agrees with the CPU atmosphere for it, the fog and the models' ambient sky color are the horizon of that same sky, and a frame that supplies no lighting (or a renderer
// without the option) draws byte-identically to the fixed-lighting renderer.

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/render/atmosphere.hpp"
#include "saige/render/camera.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 320;
constexpr uint32_t kH = 240;
constexpr double deg = 3.14159265358979323846 / 180.0;

Float3 dir_from(double elevation_deg, double azimuth_deg)
{
    const double e = elevation_deg * deg, a = azimuth_deg * deg;
    return {static_cast<float>(std::cos(e) * std::cos(a)), static_cast<float>(std::cos(e) * std::sin(a)), static_cast<float>(std::sin(e))};
}

struct Box {
    Float3 lo, hi;
};

bool ray_hits_box(Float3 origin, Float3 dir, const Box& box)
{
    float t0 = 1e-3f, t1 = std::numeric_limits<float>::max();
    const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z}, l[3] = {box.lo.x, box.lo.y, box.lo.z}, h[3] = {box.hi.x, box.hi.y, box.hi.z};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(d[i]) < 1e-9f) {
            if (o[i] < l[i] || o[i] > h[i]) return false;
            continue;
        }
        float a = (l[i] - o[i]) / d[i], b = (h[i] - o[i]) / d[i];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    return true;
}

Float3 linear_at(const Image& img, int x, int y)
{
    const uint8_t* c = img.at(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    return {srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2])};
}

/// A grey (albedo 0.5) ground and a column, on whatever device the renderer has.
struct Scene {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    std::unique_ptr<OffscreenTarget> target;
    GpuModel ground, column;
    Camera camera;
    Mat4 view_proj;
    Box column_box{{-1.0f, 11.0f, 0.0f}, {1.0f, 13.0f, 4.0f}};

    explicit Scene(const RendererSettings& settings, const char* name)
    {
        device = make_validated_device(name, /*synchronization=*/true);
        renderer = std::make_unique<SceneRenderer>(*device, rhi::Format::RGBA8_SRGB, settings);
        target = std::make_unique<OffscreenTarget>(*device, kW, kH);
        MaterialParams grey;
        grey.base_color[0] = grey.base_color[1] = grey.base_color[2] = 0.5f;
        const Float3 n{0, 0, 1};
        const float half = 120.0f;
        const std::vector<ModelVertex> v = {{{-half, -half, 0}, n, {0, 0}}, {{half, -half, 0}, n, {1, 0}}, {{half, half, 0}, n, {1, 1}}, {{-half, half, 0}, n, {0, 1}}};
        ground.name = "ground";
        ground.materials.push_back(renderer->model_renderer().create_material(grey, {}, {}, "ground"));
        ground.meshes.push_back({make_primitive(renderer->registry(), v, {0, 1, 2, 0, 2, 3}, 0)});
        ground.placements.push_back({0, Mat4::identity()});
        column = make_cube(renderer->registry(), renderer->model_renderer().create_material(grey, {}, {}, "column"));
        camera = look_at_camera({0.0f, -1.0f, 3.2f}, {0.0f, 40.0f, 0.0f});
        view_proj = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
    }
    ~Scene()
    {
        column = {};
        ground = {};
        target.reset();
        renderer.reset();
    }
    Image render(const SceneLighting* lighting, bool with_column = true)
    {
        std::vector<ModelDraw> draws{{&ground, Mat4::identity()}};
        if (with_column) draws.push_back({&column, translation({0.0f, 12.0f, 2.0f}) * scale_matrix({2.0f, 2.0f, 4.0f})});
        const Image img = target->render(*renderer, {}, camera, 0, draws, nullptr, lighting);
        CHECK(device->validation_error_count() == 0);
        return img;
    }
    std::pair<int, int> pixel_of(Float3 world) const
    {
        const Projected p = project_point(view_proj, world);
        const PixelPos px = ndc_to_pixel(p.ndc_x, p.ndc_y, static_cast<float>(kW), static_cast<float>(kH));
        return {static_cast<int>(px.x), static_cast<int>(px.y)};
    }
};

RendererSettings base_settings()
{
    RendererSettings s = deterministic_settings();
    s.fog_density = 0.0f;
    s.ground_half_size = 0.01f;  // the renderer's own checkerboard would z-fight with the model ground
    return s;
}

SceneLighting sun_at(double elevation, double azimuth, Float3 color, Float3 ambient = {0.2f, 0.18f, 0.16f})
{
    SceneLighting l;
    l.sun_direction = dir_from(elevation, azimuth) * 7.0f;  // not unit length on purpose: the renderer normalizes once, for everything
    l.sun_color = color;
    l.ambient_ground = ambient;
    return l;
}

} // namespace

TEST_CASE("Dynamic lighting: every frame's ground matches a CPU evaluation of that frame's sun, across more frames than frames in flight")
{
    RendererSettings s = base_settings();
    s.dynamic_lighting = true;
    Scene scene(s, "dyn_ground");
    const Float3 sky{s.sky.r, s.sky.g, s.sky.b};
    // Points of the ground the camera sees clearly, far from the column (no shadows in this test: they are the next one's).
    const Float3 probes[] = {{-4.0f, 5.0f, 0.0f}, {3.0f, 6.0f, 0.0f}, {-2.0f, 8.0f, 0.0f}, {5.0f, 16.0f, 0.0f}, {-6.0f, 20.0f, 0.0f}};
    const struct {
        double elevation, azimuth;
        Float3 color;
    } frames[] = {{70.0, 20.0, {1.0f, 0.95f, 0.85f}}, {45.0, 100.0, {1.6f, 1.0f, 0.5f}}, {20.0, 200.0, {0.9f, 0.4f, 0.2f}}, {8.0, 300.0, {0.3f, 0.2f, 0.1f}},
                      {60.0, 10.0, {0.0f, 0.0f, 0.0f}}, {35.0, 45.0, {1.2f, 1.2f, 1.2f}}, {80.0, 0.0, {1.0f, 0.2f, 0.9f}}};
    int checked = 0;
    for (const auto& f : frames) {
        const SceneLighting l = sun_at(f.elevation, f.azimuth, f.color);
        const Image img = scene.render(&l, /*with_column=*/false);
        const float ndl = std::max(0.0f, dir_from(f.elevation, f.azimuth).z);
        const Float3 want = (Float3{sky.x * 0.6f, sky.y * 0.6f, sky.z * 0.6f} + Float3{f.color.x * ndl, f.color.y * ndl, f.color.z * ndl}) * 0.5f;
        for (const Float3 q : probes) {
            const auto [x, y] = scene.pixel_of(q);
            const Float3 got = linear_at(img, x, y);
            const bool ok = near3(got, {std::min(want.x, 1.0f), std::min(want.y, 1.0f), std::min(want.z, 1.0f)}, 0.02f);
            if (!ok) {
                std::printf("  sun %.0f/%.0f at (%.1f, %.1f): got (%.3f %.3f %.3f) want (%.3f %.3f %.3f)\n", f.elevation, f.azimuth, static_cast<double>(q.x), static_cast<double>(q.y),
                            static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z), static_cast<double>(want.x), static_cast<double>(want.y), static_cast<double>(want.z));
            }
            CHECK(ok);
            ++checked;
        }
    }
    CHECK(checked == 7 * 5);
}

TEST_CASE("Dynamic lighting: the shadow of a column falls where an independent ray cast says for each frame's sun")
{
    RendererSettings s = base_settings();
    s.dynamic_lighting = true;
    s.shadows.enabled = true;
    s.shadows.map_size = 1024;
    s.shadows.distance = 40.0f;
    Scene scene(s, "dyn_shadow");
    const Float3 sky{s.sky.r, s.sky.g, s.sky.b};
    const Float3 ambient = Float3{sky.x * 0.6f, sky.y * 0.6f, sky.z * 0.6f};
    const struct {
        double elevation, azimuth;
    } suns[] = {{50.0, 90.0}, {38.0, 60.0}, {38.0, 120.0}, {28.0, 80.0}, {45.0, 30.0}, {33.0, 150.0}};  // the sun beyond the column, to its left and right: the shadows reach toward the camera
    int shadowed_total = 0, lit_total = 0, wrong = 0;
    std::vector<int> centroid_x;
    for (const auto& sun : suns) {
        const Float3 to_sun = dir_from(sun.elevation, sun.azimuth);
        SceneLighting l = sun_at(sun.elevation, sun.azimuth, {1.2f, 1.0f, 0.8f});
        const Image img = scene.render(&l);
        const float ndl = to_sun.z;
        const Float3 lit = (ambient + Float3{1.2f * ndl, 1.0f * ndl, 0.8f * ndl}) * 0.5f, dark = ambient * 0.5f;
        int frame_shadowed = 0;
        double sum_x = 0.0;
        for (float x = -9.0f; x <= 9.0f; x += 0.6f) {
            for (float y = 3.0f; y <= 24.0f; y += 0.6f) {
                const Float3 q{x, y, 0.0f};
                const auto in_shadow = [&](Float3 p) { return ray_hits_box(p, to_sun, scene.column_box); };
                // Away from the column's own silhouette, visible to the camera, and in the middle of its class (the filter and the pixel footprint straddle edges).
                const float pad = 0.9f;
                if (q.x > scene.column_box.lo.x - pad && q.x < scene.column_box.hi.x + pad && q.y > scene.column_box.lo.y - pad && q.y < scene.column_box.hi.y + pad) continue;
                if (ray_hits_box(q, normalize(scene.camera.position - q), scene.column_box)) continue;
                const bool is = in_shadow(q);
                bool interior = true;
                for (const Float3 d : {Float3{0.4f, 0, 0}, Float3{-0.4f, 0, 0}, Float3{0, 0.4f, 0}, Float3{0, -0.4f, 0}}) interior = interior && in_shadow(q + d) == is;
                if (!interior) continue;
                const auto [px, py] = scene.pixel_of(q);
                if (px < 1 || py < 1 || px >= static_cast<int>(kW) - 1 || py >= static_cast<int>(kH) - 1) continue;
                const Float3 got = linear_at(img, px, py);
                const bool ok = near3(got, is ? dark : lit, 0.04f);
                wrong += ok ? 0 : 1;
                if (!ok && wrong <= 6) {
                    std::printf("  sun %.0f/%.0f at (%.1f, %.1f): oracle %s, got (%.3f %.3f %.3f)\n", sun.elevation, sun.azimuth, static_cast<double>(x), static_cast<double>(y), is ? "shadow" : "lit",
                                static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z));
                }
                (is ? shadowed_total : lit_total) += 1;
                if (is) {
                    ++frame_shadowed;
                    sum_x += static_cast<double>(x);
                }
            }
        }
        std::printf("  sun %.0f/%.0f: %d shadowed probes\n", sun.elevation, sun.azimuth, frame_shadowed);
        CHECK(frame_shadowed >= 6);  // every frame's shadow is really probed
        centroid_x.push_back(frame_shadowed > 0 ? static_cast<int>(std::lround(sum_x / frame_shadowed * 10.0)) : 0);
    }
    std::printf("  %d shadowed and %d lit probes, %d wrong\n", shadowed_total, lit_total, wrong);
    CHECK(wrong == 0);
    // The shadow really moved with the sun: the six frames' shadows are not all in the same place.
    CHECK(*std::max_element(centroid_x.begin(), centroid_x.end()) - *std::min_element(centroid_x.begin(), centroid_x.end()) > 20);
}

TEST_CASE("Dynamic lighting: the sky, the fog and the models' ambient sky color follow the frame's sun")
{
    RendererSettings s = base_settings();
    s.dynamic_lighting = true;
    s.atmosphere.enabled = true;
    s.atmosphere.exposure = 3.0f;
    s.fog_density = 0.0f;
    Scene scene(s, "dyn_sky");
    // Looking at the horizon from above the ground: the upper image is sky, the lower is the ground (ambient only: the sun's color is zero).
    int probes = 0;
    for (const double elevation : {50.0, 15.0, 4.0, 40.0}) {  // four frames: the ring is reused
        SceneLighting l = sun_at(elevation, 30.0, {0.0f, 0.0f, 0.0f});
        const Image img = scene.render(&l, /*with_column=*/false);
        const Float3 sun = dir_from(elevation, 30.0);
        const float aspect = static_cast<float>(kW) / static_cast<float>(kH);
        const float tan_half = std::tan(scene.camera.fov_y_radians * 0.5f);
        for (const int y : {10, 30, 55}) {
            for (const int x : {20, 160, 300}) {
                const float nx = (static_cast<float>(x) + 0.5f) / static_cast<float>(kW) * 2.0f - 1.0f, ny = 1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(kH) * 2.0f;
                const Float3 d = normalize(camera_forward(scene.camera) + camera_right(scene.camera) * (tan_half * aspect * nx) + camera_up(scene.camera) * (tan_half * ny));
                if (d.z < 0.1f) continue;  // sky only
                const Float3 radiance = sky_radiance(s.atmosphere, d, sun);
                const Float3 want = {1.0f - std::exp(-radiance.x * s.atmosphere.exposure), 1.0f - std::exp(-radiance.y * s.atmosphere.exposure), 1.0f - std::exp(-radiance.z * s.atmosphere.exposure)};
                const Float3 got = linear_at(img, x, y);
                const bool ok = near3(got, want, 0.025f);
                if (!ok) std::printf("  sun %.0f pixel (%d, %d): got (%.3f %.3f %.3f) want (%.3f %.3f %.3f)\n", elevation, x, y, static_cast<double>(got.x), static_cast<double>(got.y),
                                     static_cast<double>(got.z), static_cast<double>(want.x), static_cast<double>(want.y), static_cast<double>(want.z));
                CHECK(ok);
                ++probes;
            }
        }
        // The ground is lit by the sky half of the ambient only, and that sky color is the horizon of this frame's sky (display-referred, as the fog is).
        const Float3 horizon = sky_horizon_color(s.atmosphere, sun);
        const Float3 fog = {1.0f - std::exp(-horizon.x), 1.0f - std::exp(-horizon.y), 1.0f - std::exp(-horizon.z)};
        const auto [gx, gy] = scene.pixel_of({0.0f, 6.0f, 0.0f});
        const Float3 got = linear_at(img, gx, gy);
        const bool ok = near3(got, {fog.x * 0.3f, fog.y * 0.3f, fog.z * 0.3f}, 0.02f);  // albedo 0.5 times 0.6 of the sky color
        if (!ok) std::printf("  sun %.0f ground: got (%.3f %.3f %.3f) want (%.3f %.3f %.3f)\n", elevation, static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z),
                             static_cast<double>(fog.x * 0.3f), static_cast<double>(fog.y * 0.3f), static_cast<double>(fog.z * 0.3f));
        CHECK(ok);
    }
    CHECK(probes >= 4 * 5);
}

TEST_CASE("Dynamic lighting: a frame without lighting, and a renderer without the option, draw byte-identically to the fixed-lighting renderer")
{
    for (const bool with_extras : {false, true}) {
        RendererSettings fixed = base_settings();
        fixed.lighting = sun_at(40.0, 250.0, {1.1f, 1.0f, 0.8f});
        fixed.lighting.sun_direction = dir_from(40.0, 250.0);
        if (with_extras) {
            fixed.shadows.enabled = true;
            fixed.shadows.map_size = 512;
            fixed.shadows.cascades = 2;
            fixed.shadows.distance = 40.0f;
            fixed.atmosphere.enabled = true;
            fixed.fog_density = 0.01f;
        }
        Scene off(fixed, "dyn_identity_off");
        const Image reference = off.render(nullptr);
        RendererSettings dynamic = fixed;
        dynamic.dynamic_lighting = true;
        Scene on(dynamic, "dyn_identity_on");
        const Image none = on.render(nullptr);                 // nothing supplied: the construction-time lighting
        const Image same = on.render(&fixed.lighting);         // the same lighting supplied explicitly (and this frame reuses the other slot)
        const Image again = on.render(nullptr);                // and the first slot again
        CHECK(none.rgba == reference.rgba);
        CHECK(same.rgba == reference.rgba);
        CHECK(again.rgba == reference.rgba);
        // The option actually does something: a different sun is a different image.
        const SceneLighting other = sun_at(15.0, 100.0, {1.0f, 0.5f, 0.2f});
        CHECK(on.render(&other).rgba != reference.rgba);
        CHECK(on.render(nullptr).rgba == reference.rgba);  // and it does not stick: the next frame without lighting is the construction-time one again
    }
}
