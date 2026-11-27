// Local lights in the real model renderer (ModelRenderSettings::local_light_capacity, the SAIGE_LOCAL_LIGHTS permutation), on a validated Vulkan device with synchronization validation.
// A grey floor drawn through the renderer is compared with the CPU oracle (saige::render_reference lights + the BRDF oracle, the same pair the compute-shader probes in test_local_lights.cpp
// use): lit image minus the same frame without lights must equal the oracle's local contribution, per channel, with the numbers printed. Also: zero lights with the permutation on is
// byte-identical to capacity 0, overflow is dropped and counted (and identical to a renderer whose capacity is exactly that), bad input changes nothing, and a run of frames with more
// frames than frames in flight each match their own lights and leave nothing behind in the next frame. CI: lavapipe, so this is correctness only; it says nothing about performance or
// about a physical GPU.

#include "saige/gfx_test/brdf_reference.hpp"
#include "saige/gfx_test/saige_test.hpp"
#include "saige/render/local_light.hpp"
#include "saige/render_reference/lights.hpp"

#include "model_test_util.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;
namespace gfx_test = saige::test;
namespace rr = saige::render_reference;

namespace {

constexpr uint32_t kW = 480;
constexpr uint32_t kH = 270;
constexpr float kAlbedo = 0.5f, kRoughness = 0.6f;

RendererSettings lit_settings(uint32_t capacity)
{
    RendererSettings s = deterministic_settings();
    s.fog_density = 0.0f;
    s.ground_half_size = 0.01f;  // the renderer's own checkerboard would z-fight with the model floor
    s.sky = {0.05f, 0.06f, 0.08f, 1.0f};
    s.model_shading = ModelShading::MetallicRoughness;
    s.dynamic_lighting = true;
    s.local_light_capacity = capacity;
    return s;
}

SceneLighting dim_sun()
{
    SceneLighting l;
    l.sun_direction = normalize(Float3{0.3f, 0.4f, 0.8f});
    l.sun_color = {0.15f, 0.14f, 0.12f};
    l.ambient_ground = {0.04f, 0.04f, 0.04f};
    return l;
}

struct Rig {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    std::unique_ptr<OffscreenTarget> target;
    GpuModel floor;
    std::vector<GpuModel> props;  // optional extra models (the capture scene), drawn with the floor
    Camera camera;
    Mat4 view_proj;
    SceneLighting sun = dim_sun();

    uint32_t width = kW, height = kH;

    Rig(const RendererSettings& settings, const char* name, uint32_t w = kW, uint32_t h = kH) : width(w), height(h)
    {
        device = make_validated_device(name, /*synchronization=*/true);
        renderer = std::make_unique<SceneRenderer>(*device, rhi::Format::RGBA8_SRGB, settings);
        target = std::make_unique<OffscreenTarget>(*device, width, height);
        MaterialParams grey;
        grey.base_color[0] = grey.base_color[1] = grey.base_color[2] = kAlbedo;
        grey.metallic = 0.0f;
        grey.roughness = kRoughness;
        const Float3 n{0, 0, 1};
        const float half = 60.0f;
        const std::vector<ModelVertex> v = {{{-half, -half, 0}, n, {0, 0}}, {{half, -half, 0}, n, {1, 0}}, {{half, half, 0}, n, {1, 1}}, {{-half, half, 0}, n, {0, 1}}};
        floor.name = "floor";
        floor.materials.push_back(renderer->model_renderer().create_material(grey, {}, {}, "floor"));
        floor.meshes.push_back({make_primitive(renderer->registry(), v, {0, 1, 2, 0, 2, 3}, 0)});
        floor.placements.push_back({0, Mat4::identity()});
        camera = look_at_camera({0.0f, -2.0f, 4.0f}, {0.0f, 12.0f, 0.0f});
        view_proj = view_projection_matrix(camera, static_cast<float>(width) / static_cast<float>(height));
    }
    ~Rig()
    {
        props.clear();
        floor = {};
        target.reset();
        renderer.reset();
    }
    Image render(std::span<const LocalLightGpu> lights = {}, FrameStats* stats = nullptr)
    {
        std::vector<ModelDraw> draws{{&floor, Mat4::identity()}};
        for (const GpuModel& extra : props) draws.push_back({&extra, Mat4::identity()});
        const Image img = target->render(*renderer, {}, camera, 0, draws, stats, &sun, lights);
        CHECK(device->validation_error_count() == 0);
        return img;
    }
    std::pair<int, int> pixel_of(Float3 world) const
    {
        const Projected p = project_point(view_proj, world);
        const PixelPos px = ndc_to_pixel(p.ndc_x, p.ndc_y, static_cast<float>(width), static_cast<float>(height));
        return {static_cast<int>(px.x), static_cast<int>(px.y)};
    }
};

Float3 linear_at(const Image& img, int x, int y)
{
    const uint8_t* c = img.at(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    return {srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2])};
}

/// The oracle: the reference light's irradiance at a floor point, then f(l, v) * E * max(n . l, 0) from the BRDF oracle.
std::array<double, 3> contribution(const LocalLightGpu& l, Float3 p, Float3 eye)
{
    const bool spot = l.direction_outer[3] != -2.0f;
    rr::LightSample sample;
    bool reached = false;
    const rr::Vec3 point{p.x, p.y, p.z};
    if (!spot) {
        rr::PointLight pl;
        pl.position = {l.position_radius[0], l.position_radius[1], l.position_radius[2]};
        pl.color = {l.color_intensity[0], l.color_intensity[1], l.color_intensity[2]};
        pl.intensity = l.color_intensity[3];
        pl.radius = l.position_radius[3];
        reached = rr::sample_point_light(pl, point, sample);
    } else {
        rr::SpotLight sl;
        sl.position = {l.position_radius[0], l.position_radius[1], l.position_radius[2]};
        sl.direction = {l.direction_outer[0], l.direction_outer[1], l.direction_outer[2]};
        sl.color = {l.color_intensity[0], l.color_intensity[1], l.color_intensity[2]};
        sl.intensity = l.color_intensity[3];
        sl.radius = l.position_radius[3];
        sl.outer_angle = std::acos(static_cast<double>(l.direction_outer[3]));
        sl.inner_angle = std::acos(static_cast<double>(l.cone[0]));
        reached = rr::sample_spot_light(sl, point, sample);
    }
    if (!reached) return {0, 0, 0};
    brdf::Surface s;
    s.base_color = {kAlbedo, kAlbedo, kAlbedo};
    s.metallic = 0.0f;
    s.roughness = kRoughness;
    const Float3 n{0, 0, 1}, v = normalize(eye - p);
    const Float3 ldir{static_cast<float>(sample.to_light[0]), static_cast<float>(sample.to_light[1]), static_cast<float>(sample.to_light[2])};
    const Float3 f = brdf::evaluate(s, n, v, ldir);
    const double ndl = std::max<double>(dot(n, ldir), 0.0);
    return {static_cast<double>(f.x) * sample.irradiance[0] * ndl, static_cast<double>(f.y) * sample.irradiance[1] * ndl, static_cast<double>(f.z) * sample.irradiance[2] * ndl};
}

std::vector<LocalLightGpu> make_lights(uint32_t shift)
{
    LocalLightSet set(64);
    const float s = static_cast<float>(shift);
    set.add_point({{-3.0f + s, 9.0f, 2.2f}, {1.0f, 0.6f, 0.3f}, 3.0f, 9.0f});
    set.add_point({{4.0f, 13.0f - s * 0.5f, 1.6f}, {0.3f, 0.6f, 1.0f}, 2.0f, 7.0f});
    SpotLightDesc sp;
    sp.position = {0.5f + s * 0.3f, 7.0f, 3.5f};
    const Float3 axis = normalize(Float3{0.0f, 0.35f, -0.94f});
    sp.direction = {axis.x, axis.y, axis.z};
    sp.color = {0.9f, 0.9f, 0.5f};
    sp.intensity = 6.0f;
    sp.radius = 10.0f;
    sp.inner_angle = 0.25f;
    sp.outer_angle = 0.55f;
    set.add_spot(sp);
    return std::vector<LocalLightGpu>(set.lights().begin(), set.lights().end());
}

/// Floor points the camera sees well, spread over the lights' reach and beyond it.
std::vector<Float3> floor_probes()
{
    std::vector<Float3> out;
    for (float y = 4.0f; y <= 22.0f; y += 1.5f) {
        for (float x = -9.0f; x <= 9.0f; x += 1.5f) out.push_back({x, y, 0.0f});
    }
    return out;
}

struct Comparison {
    int probes = 0, lit = 0, beyond = 0, wrong = 0;
    double max_err[3] = {0, 0, 0};
    double max_delta = 0.0;
};

/// lit minus baseline, against the oracle's contribution of `lights`, per channel.
Comparison compare_to_oracle(Rig& rig, const Image& lit, const Image& baseline, const std::vector<LocalLightGpu>& lights, double tolerance)
{
    Comparison c;
    const Float3 eye = rig.camera.position;
    for (const Float3 p : floor_probes()) {
        const auto [x, y] = rig.pixel_of(p);
        if (x < 1 || y < 1 || x >= static_cast<int>(kW) - 1 || y >= static_cast<int>(kH) - 1) continue;
        std::array<double, 3> want{0, 0, 0};
        for (const LocalLightGpu& l : lights) {
            const auto one = contribution(l, p, eye);
            for (int k = 0; k < 3; ++k) want[k] += one[k];
        }
        const Float3 a = linear_at(lit, x, y), b = linear_at(baseline, x, y);
        const double got[3] = {static_cast<double>(a.x - b.x), static_cast<double>(a.y - b.y), static_cast<double>(a.z - b.z)};
        ++c.probes;
        const bool reached = want[0] + want[1] + want[2] > 0.0;
        (reached ? c.lit : c.beyond) += 1;
        bool ok = true;
        for (int k = 0; k < 3; ++k) {
            const double err = std::fabs(got[k] - want[k]);
            c.max_err[k] = std::max(c.max_err[k], err);
            c.max_delta = std::max(c.max_delta, want[k]);
            ok = ok && err <= tolerance;
        }
        if (!reached) ok = ok && got[0] == 0.0 && got[1] == 0.0 && got[2] == 0.0;  // beyond every light's reach nothing may change, not even by a quantum
        if (!ok) {
            ++c.wrong;
            if (c.wrong <= 6) {
                std::printf("  floor (%.1f, %.1f): got (%.4f %.4f %.4f) want (%.4f %.4f %.4f)\n", static_cast<double>(p.x), static_cast<double>(p.y), got[0], got[1], got[2], want[0], want[1], want[2]);
            }
        }
    }
    return c;
}

bool threw_invalid(const RendererSettings& s)
{
    const auto device = make_validated_device("local_lights_ctor");
    try {
        SceneRenderer r(*device, rhi::Format::RGBA8_SRGB, s);
        (void)r.model_renderer();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

} // namespace

TEST_CASE("Local lights in the model renderer: a floor lit through the renderer matches the CPU oracle, channel by channel")
{
    Rig rig(lit_settings(16), "ll_floor");
    const Image baseline = rig.render();
    const std::vector<LocalLightGpu> lights = make_lights(0);
    FrameStats stats;
    const Image lit = rig.render(lights, &stats);
    CHECK(stats.model_local_lights_drawn == 3);
    CHECK(stats.model_local_lights_dropped == 0);
    const Comparison c = compare_to_oracle(rig, lit, baseline, lights, 0.012);
    std::printf("  %d probes (%d inside some light's reach, %d beyond all), wrong %d; max |error| per channel R %.4f G %.4f B %.4f (linear), strongest oracle contribution %.3f\n", c.probes, c.lit, c.beyond,
                c.wrong, c.max_err[0], c.max_err[1], c.max_err[2], c.max_delta);
    CHECK(c.probes >= 60);
    CHECK(c.lit >= 30);
    CHECK(c.beyond >= 5);
    CHECK(c.max_delta > 0.08);  // the comparison is not vacuous: the lights really add a visible amount
    CHECK(c.wrong == 0);
    CHECK(lit.rgba != baseline.rgba);
}

TEST_CASE("Local lights in the model renderer: zero lights with the permutation on draws byte-identically to capacity 0")
{
    Rig off(lit_settings(0), "ll_zero_off");
    const Image reference = off.render();
    Rig on(lit_settings(16), "ll_zero_on");
    const Image none = on.render();                                  // the frame publishes nothing: zero lights
    const std::vector<LocalLightGpu> empty;
    const Image empty_span = on.render(empty);                       // an empty list
    const std::vector<LocalLightGpu> black = {[] { LocalLightSet s(1); s.add_point({{0, 8, 2}, {0, 0, 0}, 0.0f, 5.0f}); return s.lights()[0]; }()};
    const Image dark = on.render(black);                             // a light that contributes nothing
    const Image after = on.render();                                 // and the other slot again
    CHECK(none.rgba == reference.rgba);
    CHECK(empty_span.rgba == reference.rgba);
    CHECK(dark.rgba == reference.rgba);
    CHECK(after.rgba == reference.rgba);
    const std::vector<LocalLightGpu> lights = make_lights(0);
    CHECK(on.render(lights).rgba != reference.rgba);  // the permutation really does something
    CHECK(on.render().rgba == reference.rgba);        // and it does not stick into the next frame
}

TEST_CASE("Local lights in the model renderer: lights beyond the capacity are dropped and counted, and the image is that of a renderer whose capacity is that count")
{
    const std::vector<LocalLightGpu> lights = make_lights(0);  // three lights
    Rig small(lit_settings(2), "ll_overflow_small");
    FrameStats stats;
    const Image dropped = small.render(lights, &stats);
    CHECK(stats.model_local_lights_drawn == 2);
    CHECK(stats.model_local_lights_dropped == 1);
    Rig exact(lit_settings(8), "ll_overflow_exact");
    FrameStats exact_stats;
    const std::vector<LocalLightGpu> first_two(lights.begin(), lights.begin() + 2);
    const Image reference = exact.render(first_two, &exact_stats);
    CHECK(exact_stats.model_local_lights_drawn == 2);
    CHECK(exact_stats.model_local_lights_dropped == 0);
    CHECK(dropped.rgba == reference.rgba);
    Rig all(lit_settings(8), "ll_overflow_all");
    CHECK(all.render(lights).rgba != dropped.rgba);  // the dropped light really was missing, not merely invisible
    // The statistics are per frame: the next frame, with nothing published, reports nothing.
    FrameStats next;
    (void)small.render({}, &next);
    CHECK(next.model_local_lights_drawn == 0);
    CHECK(next.model_local_lights_dropped == 0);
}

TEST_CASE("Local lights in the model renderer: invalid input and invalid combinations are rejected and change nothing")
{
    // Construction: capacity beyond the limit, and the combinations the first version does not support.
    RendererSettings too_many = lit_settings(kMaxLocalLights + 1);
    CHECK(threw_invalid(too_many));
    RendererSettings no_ring = lit_settings(8);
    no_ring.dynamic_lighting = false;
    CHECK(threw_invalid(no_ring));
    RendererSettings legacy = lit_settings(8);
    legacy.model_shading = ModelShading::Legacy;
    CHECK(threw_invalid(legacy));
    RendererSettings shadowed = lit_settings(8);
    shadowed.shadows.enabled = true;
    CHECK(threw_invalid(shadowed));
    RendererSettings skinned = lit_settings(8);
    skinned.skinning = true;
    CHECK(threw_invalid(skinned));
    RendererSettings instanced = lit_settings(8);
    instanced.instancing = true;
    CHECK(threw_invalid(instanced));
    CHECK_FALSE(threw_invalid(lit_settings(kMaxLocalLights)));

    // A frame's lights: every record is checked before anything is created, recorded or written.
    Rig rig(lit_settings(8), "ll_invalid");
    const Image baseline = rig.render();
    const std::vector<LocalLightGpu> good = make_lights(0);
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    const std::vector<std::function<void(LocalLightGpu&)>> breakers = {
        [&](LocalLightGpu& l) { l.position_radius[0] = nan; },     [&](LocalLightGpu& l) { l.position_radius[1] = inf; },
        [&](LocalLightGpu& l) { l.position_radius[3] = 0.0f; },    [&](LocalLightGpu& l) { l.position_radius[3] = -1.0f; },
        [&](LocalLightGpu& l) { l.position_radius[3] = 2.0e5f; },  [&](LocalLightGpu& l) { l.color_intensity[2] = -0.5f; },
        [&](LocalLightGpu& l) { l.color_intensity[3] = nan; },     [&](LocalLightGpu& l) { l.color_intensity[3] = -3.0f; },
        [&](LocalLightGpu& l) { l.cone[0] = nan; },
    };
    int rejected = 0;
    for (const auto& breaker : breakers) {
        std::vector<LocalLightGpu> bad = good;
        breaker(bad[1]);  // the second record: earlier ones are fine, so a partial write would show
        bad.resize(9, good[0]);  // and one record beyond the capacity, which is validated too
        bool threw = false;
        try {
            (void)rig.target->render(*rig.renderer, {}, rig.camera, 0, std::vector<ModelDraw>{{&rig.floor, Mat4::identity()}}, nullptr, &rig.sun, bad);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        rejected += threw ? 1 : 0;
        if (threw) {
            // The refused frame left an open device frame behind (the test's begin_frame ran first): finish it so the next render starts clean.
            (void)rig.device->end_frame();
            rig.device->wait_idle();
        }
    }
    CHECK(rejected == static_cast<int>(breakers.size()));
    CHECK(rig.device->validation_error_count() == 0);
    CHECK(rig.render().rgba == baseline.rgba);  // nothing was written by any refused frame
    // Lights for a renderer without the capacity are refused too.
    Rig plain(lit_settings(0), "ll_invalid_plain");
    bool logic = false;
    try {
        (void)plain.target->render(*plain.renderer, {}, plain.camera, 0, std::vector<ModelDraw>{{&plain.floor, Mat4::identity()}}, nullptr, &plain.sun, good);
    } catch (const std::logic_error&) {
        logic = true;
    }
    CHECK(logic);
}

TEST_CASE("Local lights in the model renderer: every frame matches its own lights across more frames than frames in flight, and the next frame starts clean")
{
    Rig rig(lit_settings(8), "ll_frames");
    const Image baseline = rig.render();
    int frames_checked = 0;
    for (uint32_t i = 0; i < 9; ++i) {  // more frames than the device has frames in flight
        const std::vector<LocalLightGpu> lights = make_lights(i);
        const Image lit = rig.render(lights);
        const Comparison c = compare_to_oracle(rig, lit, baseline, lights, 0.012);
        std::printf("  frame %u: %d probes, %d inside reach, wrong %d, max |error| R %.4f G %.4f B %.4f\n", i, c.probes, c.lit, c.wrong, c.max_err[0], c.max_err[1], c.max_err[2]);
        CHECK(c.wrong == 0);
        CHECK(c.lit >= 20);
        ++frames_checked;
        if (i % 3 == 2) CHECK(rig.render().rgba == baseline.rgba);  // a frame that publishes nothing draws none: the previous frame's lights do not linger in the slot
    }
    CHECK(frames_checked == 9);
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Local lights in the model renderer: set_local_lights itself validates before writing, publishes once per frame and refuses a renderer without the capacity")
{
    Rig rig(lit_settings(8), "ll_direct");
    ModelRenderer& mr = rig.renderer->model_renderer();
    CHECK(mr.local_light_capacity() == 8);
    const std::vector<LocalLightGpu> good = make_lights(0);
    std::vector<LocalLightGpu> bad = good;
    bad[2].position_radius[3] = std::numeric_limits<float>::quiet_NaN();
    bool threw = false;
    try {
        mr.set_local_lights(bad);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);                                   // refused before any byte was written: ...
    const Image baseline = rig.render();            // ... the frame that follows draws no light at all
    Rig reference(lit_settings(0), "ll_direct_ref");
    CHECK(baseline.rgba == reference.render().rgba);
    // Once per frame: a second publication of the same frame is refused, and does not change the first.
    mr.set_local_lights(good);
    bool twice = false;
    try {
        mr.set_local_lights(good);
    } catch (const std::logic_error&) {
        twice = true;
    }
    CHECK(twice);
    // A renderer without the capacity refuses.
    Rig plain(lit_settings(0), "ll_direct_plain");
    bool off = false;
    try {
        plain.renderer->model_renderer().set_local_lights(good);
    } catch (const std::logic_error&) {
        off = true;
    }
    CHECK(off);
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Local lights in the model renderer: with SAIGE_CAPTURE_DIR, 1920 x 1080 stills of a lit scene, the same frame without lights, and the 8x difference (lavapipe: correctness evidence, not performance)")
{
    const char* dir = std::getenv("SAIGE_CAPTURE_DIR");
    if (dir == nullptr || dir[0] == '\0') {
        std::printf("  SAIGE_CAPTURE_DIR is not set: nothing written (the comparison tests above are the gate)\n");
        return;
    }
    RendererSettings s = lit_settings(32);
    s.sky = {0.02f, 0.03f, 0.05f, 1.0f};
    Rig rig(s, "ll_capture", 1920, 1080);
    rig.sun.sun_color = {0.02f, 0.02f, 0.03f};  // a dusk: the local lights are the picture
    rig.sun.ambient_ground = {0.02f, 0.02f, 0.025f};
    MaterialParams wall;
    wall.base_color[0] = 0.75f; wall.base_color[1] = 0.72f; wall.base_color[2] = 0.68f;
    wall.metallic = 0.0f; wall.roughness = 0.7f;
    MaterialParams red = wall;
    red.base_color[0] = 0.7f; red.base_color[1] = 0.15f; red.base_color[2] = 0.12f; red.roughness = 0.35f;
    const GpuMaterial wall_m = rig.renderer->model_renderer().create_material(wall, {}, {}, "wall");
    const GpuMaterial red_m = rig.renderer->model_renderer().create_material(red, {}, {}, "red");
    struct Prop { float x, y, w, d, h; bool red; };
    const Prop props[] = {{-6.0f, 11.0f, 2.0f, 2.0f, 3.0f, false}, {-1.0f, 14.0f, 1.2f, 1.2f, 1.2f, true}, {3.5f, 10.0f, 2.5f, 1.5f, 2.2f, false}, {7.0f, 15.0f, 1.0f, 1.0f, 1.0f, true}, {0.0f, 20.0f, 9.0f, 0.6f, 3.2f, false}};
    for (const Prop& p : props) {
        GpuModel m = make_cube(rig.renderer->registry(), p.red ? red_m : wall_m);
        m.placements[0].local = translation({p.x, p.y, p.h * 0.5f}) * scale_matrix({p.w, p.d, p.h});
        rig.props.push_back(std::move(m));
    }
    LocalLightSet set(32);
    set.add_point({{-3.0f, 9.0f, 2.4f}, {1.0f, 0.62f, 0.3f}, 6.0f, 11.0f});
    set.add_point({{5.5f, 12.5f, 2.0f}, {0.3f, 0.55f, 1.0f}, 5.0f, 9.0f});
    SpotLightDesc sp;
    sp.position = {0.0f, 5.0f, 4.0f};
    const Float3 axis = normalize(Float3{0.1f, 0.55f, -0.6f});
    sp.direction = {axis.x, axis.y, axis.z};
    sp.color = {1.0f, 0.95f, 0.8f};
    sp.intensity = 14.0f;
    sp.radius = 22.0f;
    sp.inner_angle = 0.2f;
    sp.outer_angle = 0.5f;
    set.add_spot(sp);
    rig.camera = look_at_camera({0.0f, -2.0f, 3.2f}, {0.0f, 12.0f, 1.0f});
    rig.view_proj = view_projection_matrix(rig.camera, 1920.0f / 1080.0f);
    const Image off = rig.render();
    FrameStats stats;
    const Image on = rig.render(set.lights(), &stats);
    CHECK(stats.model_local_lights_drawn == 3);
    CHECK(on.rgba != off.rgba);
    std::filesystem::create_directories(dir);
    gfx_test::write_ppm(std::filesystem::path(dir) / "model_local_lights_lit_1080p.ppm", on);
    gfx_test::write_ppm(std::filesystem::path(dir) / "model_local_lights_unlit_1080p.ppm", off);
    gfx_test::write_ppm(std::filesystem::path(dir) / "model_local_lights_difference_x8_1080p.ppm", gfx_test::make_diff_image(on, off, 8));
    std::printf("  wrote 1080p stills to %s (llvmpipe)\n", dir);
    CHECK(rig.device->validation_error_count() == 0);
}
