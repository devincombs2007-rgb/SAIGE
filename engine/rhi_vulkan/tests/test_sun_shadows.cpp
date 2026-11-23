// Sun shadows for imported models on a real (validated) Vulkan device. The shadow pass, the comparison lookup and the filter are checked
// against an independent CPU ray cast: a ground point is shadowed exactly when the ray toward the sun hits the occluder, and the
// shaded pixel must be albedo * (ambient + sun term) or albedo * ambient accordingly. Also: nothing changes with shadows off, lit faces
// are not darkened by their own shadow (acne), the bias and normal offset are what prevent it, results are deterministic and validation
// stays clean. CI: lavapipe. Validation errors fail each test.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/camera.hpp"
#include "model_test_util.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 256;
constexpr uint32_t kH = 192;

RendererSettings scene_settings(bool shadows)
{
    RendererSettings s = deterministic_settings();
    s.fog_density = 0.0f;
    s.sky = {0.30f, 0.50f, 0.82f, 1.0f};
    s.shadows.enabled = shadows;
    s.shadows.map_size = 1024;
    s.shadows.distance = 12.0f;
    return s;
}

Float3 sun_direction() { return normalize(Float3{-0.40f, -0.55f, 0.73f}); }

/// Ambient light for a surface facing `n` (the shader's hemispherical term) and the sun term, so a pixel is albedo * (ambient + vis * sun).
Float3 ambient_for(Float3 n, const RendererSettings& s)
{
    const float t = n.z * 0.5f + 0.5f;
    const Float3 ground{0.20f, 0.18f, 0.16f};
    const Float3 sky{s.sky.r * 0.6f, s.sky.g * 0.6f, s.sky.b * 0.6f};
    return ground * (1.0f - t) + sky * t;
}
Float3 sun_term_for(Float3 n)
{
    const float ndl = std::max(0.0f, dot(n, sun_direction()));
    return {1.00f * ndl, 0.95f * ndl, 0.85f * ndl};
}

struct Pixel {
    int x, y;
};
Pixel to_pixel(const Mat4& vp, Float3 world)
{
    const Projected p = project_point(vp, world);
    const PixelPos px = ndc_to_pixel(p.ndc_x, p.ndc_y, static_cast<float>(kW), static_cast<float>(kH));
    return {static_cast<int>(px.x), static_cast<int>(px.y)};
}
Float3 linear_at(const Image& img, Pixel p)
{
    const uint8_t* c = img.at(static_cast<uint32_t>(p.x), static_cast<uint32_t>(p.y));
    return {srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2])};
}

/// A horizontal quad at z = 0 facing +Z, counter-clockwise seen from above.
GpuModel make_ground(GpuResourceRegistry& registry, GpuMaterial material, float half)
{
    const Float3 n{0, 0, 1};
    const std::vector<ModelVertex> v = {{{-half, -half, 0}, n, {0, 0}}, {{half, -half, 0}, n, {1, 0}}, {{half, half, 0}, n, {1, 1}}, {{-half, half, 0}, n, {0, 1}}};
    GpuModel m;
    m.name = "ground";
    m.materials.push_back(std::move(material));
    m.meshes.push_back({make_primitive(registry, v, {0, 1, 2, 0, 2, 3}, 0)});
    m.placements.push_back({0, Mat4::identity()});
    return m;
}

struct Rig {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    std::unique_ptr<OffscreenTarget> target;
    RendererSettings settings;
    GpuModel ground, cube;

    Rig(const char* name, RendererSettings s) : settings(s)
    {
        device = make_validated_device(name, /*synchronization=*/true);
        renderer = std::make_unique<SceneRenderer>(*device, rhi::Format::RGBA8_SRGB, settings);
        target = std::make_unique<OffscreenTarget>(*device, kW, kH);
        MaterialParams grey;
        grey.base_color[0] = grey.base_color[1] = grey.base_color[2] = 0.5f;
        ground = make_ground(renderer->registry(), renderer->model_renderer().create_material(grey, {}, {}, "ground"), 6.0f);
        cube = make_cube(renderer->registry(), renderer->model_renderer().create_material(grey, {}, {}, "cube"));
    }
    ~Rig()
    {
        cube = {};
        ground = {};
        target.reset();
        renderer.reset();
    }

    /// The cube (edge 1) resting on the ground at the origin: it occupies x, y in [-0.5, 0.5] and z in [0, 1].
    ModelDraw cube_draw() const { return {&cube, translation({0.0f, 0.0f, 0.5f})}; }
    ModelDraw ground_draw() const { return {&ground, Mat4::identity()}; }

    Image render(const Camera& camera, std::span<const ModelDraw> draws, FrameStats* stats = nullptr)
    {
        return target->render(*renderer, {}, camera, 0, draws, stats);
    }
};

// From the +X, -Y side, so the shadow (which falls toward +X, +Y, away from the sun) is on the visible side of the cube.
Camera scene_camera() { return look_at_camera({5.0f, -4.0f, 5.0f}, {0.6f, 0.6f, 0.0f}); }

/// Does the ray from `origin` along `dir` hit the axis-aligned box [lo, hi] at t > epsilon? (slab method; an independent oracle)
bool ray_hits_box(Float3 origin, Float3 dir, Float3 lo, Float3 hi)
{
    float t0 = 1e-3f, t1 = std::numeric_limits<float>::max();
    const float o[3] = {origin.x, origin.y, origin.z}, d[3] = {dir.x, dir.y, dir.z}, l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
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

bool in_shadow(Float3 q) { return ray_hits_box(q, sun_direction(), {-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 1.0f}); }

int max_channel_difference(const Image& a, const Image& b)
{
    int worst = 0;
    for (uint32_t y = 0; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) worst = std::max(worst, max_channel_delta(a.at(x, y), b.at(x, y)));
    }
    return worst;
}

} // namespace

TEST_CASE("Sun shadows: the shadow of a cube on the ground matches an independent CPU ray cast")
{
    Rig rig("sun_shadow_cube", scene_settings(true));
    const Camera camera = scene_camera();
    const ModelDraw draws[] = {rig.ground_draw(), rig.cube_draw()};
    FrameStats stats;
    const Image img = rig.render(camera, draws, &stats);
    CHECK(rig.device->validation_error_count() == 0);
    CHECK(stats.shadow_draw_calls == 2);  // the ground and the cube are both casters

    const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
    const Float3 up{0, 0, 1};
    const Float3 lit = (ambient_for(up, rig.settings) + sun_term_for(up)) * 0.5f;
    const Float3 dark = ambient_for(up, rig.settings) * 0.5f;
    int shadowed = 0, sunlit = 0, wrong = 0;
    // A grid of ground points around the cube, including points hugging its base. A point counts only if its shadow classification is
    // the same for every neighbor within 12 cm (the filter and the bias make the edge soft; the interior must be exact).
    for (int ix = -28; ix <= 28; ++ix) {
        for (int iy = -28; iy <= 28; ++iy) {
            const Float3 q{static_cast<float>(ix) * 0.1f, static_cast<float>(iy) * 0.1f, 0.0f};
            if (std::fabs(q.x) < 0.56f && std::fabs(q.y) < 0.56f) continue;  // under or against the cube: hidden from the camera or on its edge
            // The cube must not hide this ground point from the camera, nor any point within the pixel's footprint (silhouette pixels show the cube).
            bool visible = true;
            for (const Float3 d : {Float3{0, 0, 0}, Float3{0.1f, 0, 0}, Float3{-0.1f, 0, 0}, Float3{0, 0.1f, 0}, Float3{0, -0.1f, 0}}) {
                visible = visible && !ray_hits_box(q + d, normalize(camera.position - (q + d)), {-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 1.0f});
            }
            if (!visible) continue;
            const bool s = in_shadow(q);
            bool interior = true;
            for (const Float3 d : {Float3{0.12f, 0, 0}, Float3{-0.12f, 0, 0}, Float3{0, 0.12f, 0}, Float3{0, -0.12f, 0}}) {
                interior = interior && (in_shadow(q + d) == s);
            }
            if (!interior) continue;
            const Pixel px = to_pixel(vp, q);
            if (px.x < 0 || px.y < 0 || px.x >= static_cast<int>(kW) || px.y >= static_cast<int>(kH)) continue;
            const Float3 got = linear_at(img, px);
            const bool ok = near3(got, s ? dark : lit, 0.03f);
            (s ? shadowed : sunlit) += 1;
            wrong += ok ? 0 : 1;
            if (!ok) {
                std::printf("  wrong at (%.1f, %.1f): oracle says %s, pixel (%.3f, %.3f, %.3f)\n", static_cast<double>(q.x), static_cast<double>(q.y), s ? "shadow" : "lit",
                            static_cast<double>(got.x), static_cast<double>(got.y), static_cast<double>(got.z));
            }
        }
    }
    CHECK(shadowed >= 20);
    CHECK(sunlit >= 150);
    CHECK(wrong == 0);

    // The top of the cube faces the sun: it is lit (not darkened by its own shadow), and the face turned away is unlit by the sun regardless.
    const Float3 top_lit = (ambient_for(up, rig.settings) + sun_term_for(up)) * 0.5f;
    CHECK(near3(linear_at(img, to_pixel(vp, {0.1f, 0.05f, 1.0f})), top_lit, 0.03f));
}

TEST_CASE("Sun shadows: off is exactly the image without them, and lit surfaces are not shadowed by themselves (no acne)")
{
    const Camera camera = scene_camera();
    // Ground only, and cube only: with no other occluder every pixel is sunlit or faces away, so shadows on and off must agree.
    for (const bool with_ground : {true, false}) {
        Rig on("sun_shadow_acne_on", scene_settings(true));
        Rig off("sun_shadow_acne_off", scene_settings(false));
        std::vector<ModelDraw> draws_on, draws_off;
        if (with_ground) {
            draws_on.push_back(on.ground_draw());
            draws_off.push_back(off.ground_draw());
        } else {
            draws_on.push_back(on.cube_draw());
            draws_off.push_back(off.cube_draw());
        }
        FrameStats stats_off;
        const Image shadowed = on.render(camera, draws_on);
        const Image plain = off.render(camera, draws_off, &stats_off);
        CHECK(max_channel_difference(shadowed, plain) <= 1);
        CHECK(stats_off.shadow_draw_calls == 0);
        CHECK(on.device->validation_error_count() == 0);
        CHECK(off.device->validation_error_count() == 0);
    }
    // The bias and the normal offset are what prevent acne: without them the same ground plane shadows itself.
    RendererSettings bare = scene_settings(true);
    bare.shadows.depth_bias_constant = 0.0f;
    bare.shadows.depth_bias_slope = 0.0f;
    bare.shadows.normal_offset_texels = 0.0f;
    bare.shadows.filter_radius = 0;
    Rig acne("sun_shadow_acne_bare", bare);
    Rig reference("sun_shadow_acne_reference", scene_settings(false));
    const ModelDraw a[] = {acne.ground_draw()};
    const ModelDraw r[] = {reference.ground_draw()};
    const int acne_difference = max_channel_difference(acne.render(camera, a), reference.render(camera, r));
    CHECK(acne_difference > 20);  // visible self-shadowing: the test above is sensitive to what it claims to test
}

TEST_CASE("Sun shadows: deterministic, and the shadow follows a moving occluder")
{
    Rig rig("sun_shadow_determinism", scene_settings(true));
    const Camera camera = scene_camera();
    const ModelDraw draws[] = {rig.ground_draw(), rig.cube_draw()};
    const Image first = rig.render(camera, draws);
    const Image second = rig.render(camera, draws);
    CHECK(first.rgba == second.rgba);  // every frame of a fixed scene is bit-identical (the per-frame shadow ring does not leak state)

    // Lift the cube off the ground: its shadow moves away from its base, so the ground point that was shadowed beside it is now lit.
    const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
    const Float3 up{0, 0, 1};
    const Float3 lit = (ambient_for(up, rig.settings) + sun_term_for(up)) * 0.5f;
    const Float3 dark = ambient_for(up, rig.settings) * 0.5f;
    const Float3 beside{0.62f, 0.55f, 0.0f};  // on the shadow side, close to the base: inside the shadow of the resting cube
    REQUIRE(in_shadow(beside));
    CHECK(near3(linear_at(first, to_pixel(vp, beside)), dark, 0.03f));
    const ModelDraw lifted[] = {rig.ground_draw(), {&rig.cube, translation({0.0f, 0.0f, 3.5f})}};
    const Image moved = rig.render(camera, lifted);
    CHECK(near3(linear_at(moved, to_pixel(vp, beside)), lit, 0.03f));
    // The new shadow lies 3 m along the shadow direction: (x, y) = -L.xy * (h / L.z) from the cube's center.
    const Float3 l = sun_direction();
    const Float3 center{-l.x * (3.5f / l.z), -l.y * (3.5f / l.z), 0.0f};
    CHECK(near3(linear_at(moved, to_pixel(vp, center)), dark, 0.03f));
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Sun shadows: the percentage-closer filter softens the edge by its radius")
{
    const Camera camera = scene_camera();
    const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
    const auto penumbra = [&](uint32_t radius) {
        RendererSettings s = scene_settings(true);
        s.shadows.filter_radius = radius;
        Rig rig("sun_shadow_filter", s);
        const ModelDraw draws[] = {rig.ground_draw(), rig.cube_draw()};
        const Image img = rig.render(camera, draws);
        CHECK(rig.device->validation_error_count() == 0);
        const Float3 up{0, 0, 1};
        const float dark = ambient_for(up, rig.settings).x * 0.5f;
        const float lit = (ambient_for(up, rig.settings) + sun_term_for(up)).x * 0.5f;
        int partial = 0;
        for (int ix = 0; ix < 90; ++ix) {
            for (int iy = 0; iy < 90; ++iy) {  // ground points in a window around the visible shadow edges, 2 cm apart
                const Float3 q{0.4f + 0.02f * static_cast<float>(ix), -0.2f + 0.02f * static_cast<float>(iy), 0.0f};
                const Pixel px = to_pixel(vp, q);
                if (px.x < 0 || px.y < 0 || px.x >= static_cast<int>(kW) || px.y >= static_cast<int>(kH)) continue;
                if (ray_hits_box(q, normalize(camera.position - q), {-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 1.0f})) continue;
                const float v = linear_at(img, px).x;
                if (v > dark + 0.04f && v < lit - 0.04f) ++partial;
            }
        }
        return partial;
    };
    const int sharp = penumbra(0), soft = penumbra(4);
    CHECK(soft > sharp + 20);  // a wider filter leaves many more pixels between fully lit and fully shadowed
}

TEST_CASE("Sun shadows: invalid settings are rejected before anything is created")
{
    const auto device = make_validated_device("sun_shadow_settings");
    const auto rejected = [&](auto&& change) {
        RendererSettings s = scene_settings(true);
        change(s);
        try {
            SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, s);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    constexpr float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(!rejected([](RendererSettings&) {}));
    CHECK(rejected([](RendererSettings& s) { s.shadows.map_size = 8; }));
    CHECK(rejected([](RendererSettings& s) { s.shadows.map_size = 16384; }));
    CHECK(rejected([](RendererSettings& s) { s.shadows.distance = nan; }));
    CHECK(rejected([](RendererSettings& s) { s.shadows.distance = 0.05f; }));  // not beyond the near plane
    CHECK(rejected([](RendererSettings& s) { s.shadows.caster_margin = -1.0f; }));
    CHECK(rejected([](RendererSettings& s) { s.shadows.depth_bias_constant = nan; }));
    CHECK(rejected([](RendererSettings& s) { s.shadows.depth_bias_slope = std::numeric_limits<float>::infinity(); }));
    CHECK(rejected([](RendererSettings& s) { s.shadows.normal_offset_texels = -0.5f; }));
    CHECK(rejected([](RendererSettings& s) { s.shadows.filter_radius = 9; }));
    // Disabled shadows are not validated: their other fields are irrelevant.
    CHECK(!rejected([](RendererSettings& s) { s.shadows.enabled = false; s.shadows.map_size = 3; }));
    CHECK(device->validation_error_count() == 0);
}
