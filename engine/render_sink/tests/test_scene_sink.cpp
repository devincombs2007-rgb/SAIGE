// End-to-end on Vulkan: Core's saved textured project (ProjectSession: saved camera, authoritative world matrices, stable
// asset IDs) -> GPU staging -> RendererFrameSink -> pixels. Exit code 77 when no Vulkan 1.3 device is available.
// Validation errors, including synchronization hazards, fail each test. Set SAIGE_CAPTURE_DIR to save frames as PPM.

#include "saige/gfx_assets/image.hpp"
#include "saige/gfx_test/image.hpp"
#include "saige/gfx_test/png.hpp"
#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/gltf.hpp"
#include "saige/project_session.hpp"
#include "saige/render/camera.hpp"
#include "saige/render_sink/renderer_frame_sink.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace saige;
using namespace saige::render;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kW = 320;
constexpr uint32_t kH = 180;

std::unique_ptr<rhi::Device> make_device(const char* name)
{
    rhi::DeviceDesc desc;
    desc.enable_validation = true;
    desc.enable_synchronization_validation = true;
    const char* require = std::getenv("SAIGE_REQUIRE_VALIDATION");
    desc.require_validation = require != nullptr && require[0] != '\0' && require[0] != '0';
    desc.application_name = name;
    return rhi::create_vulkan_device(desc);
}

FrameSinkSettings sink_settings()
{
    FrameSinkSettings s;
    s.renderer.fog_density = 0.0f;  // lit color is then albedo * K, independent of distance
    s.renderer.spin_radians_per_second = 0.0f;
    return s;
}

/// Core's saved scene, loaded the way the app loads it.
struct Scene {
    ProjectSession session;
    gfx::GltfAsset asset;
    saige::AssetId id;
    std::vector<RenderInstance> instances;
    CameraView camera;

    Scene() : session(fs::path(SAIGE_TEXTURED_SCENE_DIR) / "saige.scene.json")
    {
        REQUIRE(session.source().assets.size() == 1);
        id = session.source().assets.front().id;
        const auto relative = fs::path(std::u8string(session.source().assets.front().relative_path.begin(),
                                                     session.source().assets.front().relative_path.end()));
        const gfx::GltfLoadResult loaded = gfx::load_gltf_file(session.root() / relative);
        REQUIRE(loaded.ok());
        asset = *loaded.asset;
        instances = session.world().snapshot();
        REQUIRE(session.camera_view().has_value());
        camera = *session.camera_view();
    }

    [[nodiscard]] std::vector<SceneAssetRef> refs(const gfx::GltfAsset* override_asset = nullptr) const
    {
        return {{id, override_asset != nullptr ? override_asset : &asset, "checker-box"}};
    }
};

void submit(RendererFrameSink& sink, std::span<const RenderInstance> instances, const std::optional<CameraView>& camera, uint64_t tick = 0)
{
    FrameView view{tick, 0.0, instances, camera};
    sink.submit(view);
}

saige::test::Image read(RendererFrameSink& sink)
{
    saige::test::Image img;
    img.width = sink.width();
    img.height = sink.height();
    img.rgba = sink.read_back_rgba();
    return img;
}

void capture(const saige::test::Image& img, const char* name)
{
    if (const char* dir = std::getenv("SAIGE_CAPTURE_DIR")) {
        (void)saige::test::write_ppm(std::string(dir) + "/" + name + ".ppm", img);
    }
}

float srgb_to_linear(uint8_t v)
{
    const float c = static_cast<float>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

Float3 lighting_k(Float3 n, const RendererSettings& s)
{
    const Float3 sun = normalize(Float3{-0.40f, -0.55f, 0.73f});
    const float ndl = std::max(0.0f, dot(n, sun));
    const float t = n.z * 0.5f + 0.5f;
    const Float3 ground{0.20f, 0.18f, 0.16f};
    const Float3 sky{s.sky.r * 0.6f, s.sky.g * 0.6f, s.sky.b * 0.6f};
    const Float3 ambient = ground * (1.0f - t) + sky * t;
    return {ambient.x + 1.00f * ndl, ambient.y + 0.95f * ndl, ambient.z + 0.85f * ndl};
}

Float3 linear_at(const saige::test::Image& img, int x, int y)
{
    const uint8_t* c = img.at(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    return {srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2])};
}

bool near3(Float3 a, Float3 b, float tolerance)
{
    return std::fabs(a.x - b.x) <= tolerance && std::fabs(a.y - b.y) <= tolerance && std::fabs(a.z - b.z) <= tolerance;
}

Float3 texel_linear(bool orange)
{
    return orange ? Float3{srgb_to_linear(232), srgb_to_linear(108), srgb_to_linear(37)}
                  : Float3{srgb_to_linear(34), srgb_to_linear(130), srgb_to_linear(186)};
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

RenderInstance make_instance(saige::AssetId mesh, const Mat4& world, uint32_t index)
{
    RenderInstance inst;
    inst.entity = {index, 1};
    inst.mesh = mesh;
    saige::WorldMatrix wm{};
    for (std::size_t i = 0; i < 16; ++i) wm[i] = world.m[i];
    inst.world_matrix = wm;
    inst.position = {world.at(0, 3), world.at(1, 3), world.at(2, 3)};
    return inst;
}

Mat4 translation(Float3 t)
{
    Mat4 m = Mat4::identity();
    m.at(0, 3) = t.x;
    m.at(1, 3) = t.y;
    m.at(2, 3) = t.z;
    return m;
}

/// Pixels that differ from `other` by more than `threshold` in any channel, and their centroid.
struct Footprint {
    uint32_t count = 0;
    double cx = 0, cy = 0;
};
Footprint footprint(const saige::test::Image& a, const saige::test::Image& other, int threshold = 20)
{
    Footprint f;
    for (uint32_t y = 0; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) {
            int delta = 0;
            for (int c = 0; c < 3; ++c) delta = std::max(delta, std::abs(int{a.at(x, y)[c]} - int{other.at(x, y)[c]}));
            if (delta > threshold) {
                ++f.count;
                f.cx += x + 0.5;
                f.cy += y + 0.5;
            }
        }
    }
    if (f.count > 0) {
        f.cx /= f.count;
        f.cy /= f.count;
    }
    return f;
}

} // namespace

TEST_CASE("Scene: Core's saved textured project renders with its texture, three placements and saved camera")
{
    Scene scene;
    auto sink = std::make_unique<RendererFrameSink>(make_device("scene_texture"), kW, kH, sink_settings());
    StagedScene staged = sink->stage_scene(scene.refs());
    CHECK(staged.model_count() == 1);
    CHECK(staged.report().textures == 1);
    CHECK(staged.report().warnings.empty());
    sink->activate_scene(std::move(staged));
    CHECK(sink->active_model_count() == 1);

    submit(*sink, scene.instances, scene.camera);
    CHECK(sink->stats().model_instances_drawn == 3);  // the saved scene's three nodes, one shared GPU model
    CHECK(sink->stats().unresolved_instances == 0);
    const saige::test::Image img = read(*sink);
    capture(img, "scene_textured");

    // Exact texture check on the unrotated, unscaled-by-parent root box's face toward the camera (-X). For each of the 4x4
    // texture cells, take the point at the cell's UV center, find its world position from the model's own vertices, project
    // it, and compare against the cell's color times the lighting for that face. Wrong orientation, mapping or lighting fails.
    const Camera camera = to_render_camera(scene.camera);
    const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
    const gfx::Primitive& prim = scene.asset.meshes[0].primitives[0];
    Float3 corner[2][2];  // [v][u] corners of the -X face, from the model's own vertices
    int found = 0;
    for (uint32_t v = 0; v < prim.vertex_count(); ++v) {
        const Float3 n{prim.normals[v * 3], prim.normals[v * 3 + 1], prim.normals[v * 3 + 2]};
        if (n.x < -0.99f) {
            const int iu = prim.uv0[v * 2] > 0.5f ? 1 : 0;
            const int iv = prim.uv0[v * 2 + 1] > 0.5f ? 1 : 0;
            corner[iv][iu] = {prim.positions[v * 3], prim.positions[v * 3 + 1], prim.positions[v * 3 + 2]};
            ++found;
        }
    }
    REQUIRE(found == 4);
    // The instance at the origin is the root node (scale 1.25, no rotation): its -X face points straight at the camera.
    Mat4 root = Mat4::identity();
    bool have_root = false;
    for (const RenderInstance& inst : scene.instances) {
        const Mat4 m = to_render_matrix(*inst.world_matrix);
        if (std::fabs(m.at(0, 3)) < 1e-4f && std::fabs(m.at(1, 3)) < 1e-4f && std::fabs(m.at(2, 3)) < 1e-4f) {
            root = m;
            have_root = true;
        }
    }
    REQUIRE(have_root);
    const Float3 k = lighting_k({-1, 0, 0}, sink_settings().renderer);
    int bad = 0, checked = 0;
    for (int cy = 0; cy < 4; ++cy) {
        for (int cx = 0; cx < 4; ++cx) {
            const float u = (static_cast<float>(cx) + 0.5f) / 4.0f;
            const float v = (static_cast<float>(cy) + 0.5f) / 4.0f;
            const Float3 local = corner[0][0] + (corner[0][1] - corner[0][0]) * u + (corner[1][0] - corner[0][0]) * v;
            const Float3 world = transform_point(root, local);
            if (world.z < 0.1f) {
                continue;  // the fixture's box is centered on z = 0, so the lower half of the face is below the ground plane
            }
            const Pixel p = to_pixel(vp, world);
            const Float3 want = texel_linear(((cx + cy) % 2) == 0);
            ++checked;
            bad += near3(linear_at(img, p.x, p.y), {want.x * k.x, want.y * k.y, want.z * k.z}, 0.04f) ? 0 : 1;
        }
    }
    CHECK(checked >= 8);  // at least the upper two rows of cells are above ground
    CHECK(bad == 0);

    // The other two placements show the checker too (both colors appear), not a flat color.
    size_t orange = 0, blue = 0;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const Float3 c = linear_at(img, static_cast<int>(x), static_cast<int>(y));
            orange += (c.x > c.z * 3.0f && c.x > 0.15f) ? 1u : 0u;
            blue += (c.z > c.x * 1.5f && c.z > 0.12f && c.z > c.y * 0.9f) ? 1u : 0u;
        }
    }
    CHECK(orange > 60);
    CHECK(blue > 60);
    sink.reset();
}

TEST_CASE("Scene: overlapping instances occlude identically in any draw order, nearest wins, and the camera moves the view")
{
    Scene scene;
    RendererFrameSink sink(make_device("scene_occlusion"), kW, kH, sink_settings());
    sink.activate_scene(sink.stage_scene(scene.refs()));
    const std::optional<CameraView> cam = scene.camera;

    // Two boxes on the view axis, the far one offset so they overlap on screen.
    const RenderInstance near_box = make_instance(scene.id, translation({0, 0, 0}), 1);
    const RenderInstance far_box = make_instance(scene.id, translation({2.0f, 0.5f, 0}), 2);
    const RenderInstance forward[2] = {near_box, far_box};
    const RenderInstance reversed[2] = {far_box, near_box};
    submit(sink, forward, cam);
    const saige::test::Image a = read(sink);
    submit(sink, reversed, cam);
    const saige::test::Image b = read(sink);
    CHECK(a.rgba == b.rgba);  // draw order cannot change the result: depth decides
    capture(a, "scene_occlusion");

    // Where they overlap the near box wins: those pixels equal the near box rendered alone.
    submit(sink, std::span(&near_box, 1), cam);
    const saige::test::Image near_alone = read(sink);
    submit(sink, std::span(&far_box, 1), cam);
    const saige::test::Image far_alone = read(sink);
    size_t overlap = 0, near_wins = 0;
    submit(sink, {}, cam);
    const saige::test::Image empty = read(sink);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const bool near_covers = std::memcmp(near_alone.at(x, y), empty.at(x, y), 3) != 0;
            const bool far_covers = std::memcmp(far_alone.at(x, y), empty.at(x, y), 3) != 0;
            if (near_covers && far_covers) {
                ++overlap;
                near_wins += std::memcmp(a.at(x, y), near_alone.at(x, y), 3) == 0 ? 1u : 0u;
            }
        }
    }
    CHECK(overlap > 100);  // the test is not vacuous: the boxes really overlap on screen
    CHECK(near_wins == overlap);

    // Camera movement: move the camera 1 m to its left (+Y). The view changes as the projection math predicts.
    CameraView moved = scene.camera;
    moved.position.y += 1.0f;
    submit(sink, std::span(&near_box, 1), moved);
    const saige::test::Image after = read(sink);
    submit(sink, {}, moved);
    const saige::test::Image empty_moved = read(sink);
    const Footprint before_fp = footprint(near_alone, empty);
    const Footprint after_fp = footprint(after, empty_moved);
    CHECK(before_fp.count > 200);
    CHECK(after_fp.count > 200);
    const float aspect = static_cast<float>(kW) / static_cast<float>(kH);
    const Float3 center{0, 0, 0};
    const Pixel p0 = to_pixel(view_projection_matrix(to_render_camera(scene.camera), aspect), center);
    const Pixel p1 = to_pixel(view_projection_matrix(to_render_camera(moved), aspect), center);
    // The camera moved left, so the box shifts right on screen by the amount the projection predicts.
    CHECK((after_fp.cx - before_fp.cx) > 10.0);
    CHECK(std::fabs((after_fp.cx - before_fp.cx) - static_cast<double>(p1.x - p0.x)) < 3.0);
    CHECK(footprint(after, near_alone).count > 500);  // and the picture visibly changed
    capture(after, "scene_camera_moved");
    CHECK(sink.device().validation_error_count() == 0);
}

TEST_CASE("Scene: a failed staging leaves the active scene drawing exactly as before")
{
    Scene scene;
    RendererFrameSink sink(make_device("scene_failed_stage"), kW, kH, sink_settings());
    sink.activate_scene(sink.stage_scene(scene.refs()));
    submit(sink, scene.instances, scene.camera);
    const saige::test::Image before = read(sink);
    const uint32_t live_before = sink.renderer_mutable().registry().total_stats().live;

    gfx::GltfAsset broken = scene.asset;
    broken.images[0].bytes.resize(24);  // a truncated PNG
    std::string message;
    try {
        (void)sink.stage_scene(scene.refs(&broken));
    } catch (const std::runtime_error& e) {
        message = e.what();
    }
    CHECK(message.find("checker-box/image[0]") != std::string::npos);  // actionable: which asset and image
    CHECK(sink.renderer_mutable().registry().total_stats().live == live_before);  // the candidate left nothing behind

    submit(sink, scene.instances, scene.camera);
    CHECK(read(sink).rgba == before.rgba);  // the old scene still draws, byte for byte

    // Invalid requests are rejected before any GPU work.
    saige::AssetId zero{};
    const SceneAssetRef builtin{zero, &scene.asset, "builtin"};
    CHECK_FALSE(([&] { try { (void)sink.stage_scene(std::span(&builtin, 1)); } catch (const std::invalid_argument&) { return false; } return true; })());
    const SceneAssetRef dup[2] = {{scene.id, &scene.asset, "a"}, {scene.id, &scene.asset, "b"}};
    CHECK_FALSE(([&] { try { (void)sink.stage_scene(dup); } catch (const std::invalid_argument&) { return false; } return true; })());
    CHECK(sink.device().validation_error_count() == 0);
}

TEST_CASE("Scene: replacing the scene while frames are in flight retires the old resources and shows the new one")
{
    Scene scene;
    RendererFrameSink sink(make_device("scene_replace"), kW, kH, sink_settings());
    sink.activate_scene(sink.stage_scene(scene.refs()));

    // The replacement: the same asset with its material tinted green, staged completely before the swap.
    gfx::GltfAsset green = scene.asset;
    green.materials[0].base_color[0] = 0.15f;
    green.materials[0].base_color[1] = 1.0f;
    green.materials[0].base_color[2] = 0.15f;
    StagedScene candidate = sink.stage_scene(scene.refs(&green));

    for (uint64_t i = 0; i < 3; ++i) {
        submit(sink, scene.instances, scene.camera, i);  // frames using the old scene, not waited for
    }
    sink.activate_scene(std::move(candidate));  // the old scene's references drop with those frames possibly still running
    submit(sink, scene.instances, scene.camera, 3);
    const saige::test::Image after = read(sink);
    capture(after, "scene_replaced_green");

    // The new scene is visibly the green-tinted one: green dominates where the box is drawn.
    size_t greenish = 0, orangeish = 0;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const Float3 c = linear_at(after, static_cast<int>(x), static_cast<int>(y));
            greenish += (c.y > c.x * 1.6f && c.y > c.z * 1.6f && c.y > 0.1f) ? 1u : 0u;
            orangeish += (c.x > c.z * 3.0f && c.x > 0.15f) ? 1u : 0u;
        }
    }
    CHECK(greenish > 300);
    CHECK(orangeish == 0);  // nothing of the old orange texture remains

    // Counters balance after idle: exactly one scene's worth of textures remains (the new checker plus the white fallback).
    const ResourceStats textures = sink.renderer_mutable().registry().stats(ResourceKind::Texture);
    CHECK(textures.live == 2);
    CHECK(textures.created == 3);    // white fallback, old checker, new checker
    CHECK(textures.destroyed == 1);  // the old checker, released at the swap
    CHECK(sink.device().validation_error_count() == 0);
}

TEST_CASE("Scene: an instance whose asset is not staged draws a fallback cube and is counted")
{
    Scene scene;
    RendererFrameSink sink(make_device("scene_unresolved"), kW, kH, sink_settings());
    // Nothing staged: every instance names an unknown asset.
    submit(sink, scene.instances, scene.camera);
    CHECK(sink.stats().unresolved_instances == scene.instances.size());
    CHECK(sink.stats().model_instances_drawn == 0);
    const saige::test::Image img = read(sink);
    submit(sink, {}, scene.camera);
    CHECK(footprint(img, read(sink)).count > 300);  // cubes, not nothing
    CHECK(sink.device().validation_error_count() == 0);
}

TEST_CASE("Scene: shutdown with an active scene is leak-free")
{
    Scene scene;
    std::string report;
    {
        RendererFrameSink sink(make_device("scene_shutdown"), kW, kH, sink_settings());
        sink.renderer_mutable().registry().set_leak_sink([&](std::string_view m) { report = std::string(m); });
        sink.activate_scene(sink.stage_scene(scene.refs()));
        for (uint64_t i = 0; i < 3; ++i) {
            submit(sink, scene.instances, scene.camera, i);  // frames still in flight when the sink goes away
        }
    }
    if (!report.empty()) std::printf("%s", report.c_str());
    CHECK(report.empty());
}

namespace {

double median(std::vector<double> v)
{
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[v.size() / 2];
}

/// Submits `frames` empty-scene frames at the given size and returns the distinct GPU scene-pass times the sink reported.
std::vector<double> scene_pass_times(uint32_t w, uint32_t h, const Scene& scene, int frames)
{
    RendererFrameSink sink(make_device("scene_timing"), w, h, sink_settings());
    sink.activate_scene(sink.stage_scene(scene.refs()));
    std::vector<double> times;
    uint64_t last_frame = ~uint64_t{0};
    for (int i = 0; i < frames; ++i) {
        submit(sink, scene.instances, scene.camera, static_cast<uint64_t>(i));
        const RendererDiagnostics d = sink.diagnostics();
        if (d.gpu_scene_pass_ms && d.gpu_timing_frame != last_frame) {
            times.push_back(*d.gpu_scene_pass_ms);
            last_frame = d.gpu_timing_frame;
        }
    }
    return times;
}

} // namespace

TEST_CASE("Scene diagnostics: GPU timing follows the work, and is absent (not zero) where unsupported")
{
    Scene scene;
    {
        RendererFrameSink sink(make_device("scene_diag"), kW, kH, sink_settings());
        const RendererDiagnostics idle = sink.diagnostics();
        CHECK(!idle.adapter.empty());
        CHECK_FALSE(idle.gpu_scene_pass_ms.has_value());  // nothing has completed yet
        CHECK(idle.validation_errors == 0);
        sink.activate_scene(sink.stage_scene(scene.refs()));
        for (uint64_t i = 0; i < 8; ++i) submit(sink, scene.instances, scene.camera, i);
        (void)read(sink);
        const RendererDiagnostics d = sink.diagnostics();
        std::printf("  adapter '%s' software=%d timestamps=%d scene pass=%s\n", d.adapter.c_str(), d.software_device ? 1 : 0,
                    d.gpu_timestamps_supported ? 1 : 0, d.gpu_scene_pass_ms ? std::to_string(*d.gpu_scene_pass_ms).c_str() : "unavailable");
        const std::string json = to_json(d);
        CHECK(json.find("\"schema_version\":1") != std::string::npos);
        if (d.gpu_timestamps_supported) {
            REQUIRE(d.gpu_scene_pass_ms.has_value());
            CHECK(*d.gpu_scene_pass_ms >= 0.0);
            CHECK(d.gpu_timing_frame < 8);
            CHECK(json.find("\"gpu_scene_pass_ms\":null") == std::string::npos);
        } else {
            CHECK_FALSE(d.gpu_scene_pass_ms.has_value());
            CHECK(json.find("\"gpu_scene_pass_ms\":null") != std::string::npos);
        }
        CHECK(d.validation_errors == 0);
    }

    // The measurement follows the work: a 640x360 pass costs clearly more than a 32x18 one (medians over frames). On a
    // software rasterizer this is CPU time, which the adapter name and `software_device` make explicit.
    if (!make_device("scene_timing_probe")->info().gpu_timestamps) {
        std::printf("  (timestamps unavailable: skipping the scaling check)\n");
        return;
    }
    const std::vector<double> small = scene_pass_times(32, 18, scene, 12);
    const std::vector<double> large = scene_pass_times(640, 360, scene, 12);
    REQUIRE(small.size() >= 4);
    REQUIRE(large.size() >= 4);
    std::printf("  median scene pass: %.4f ms at 32x18, %.4f ms at 640x360\n", median(small), median(large));
    CHECK(median(large) > 1.5 * median(small));
}

TEST_CASE("Scene diagnostics: device memory rises when a scene is staged and returns exactly when it is released")
{
    Scene scene;
    RendererFrameSink sink(make_device("scene_diag_memory"), kW, kH, sink_settings());
    const auto settle = [&] {  // let the device retire everything released so far
        sink.activate_scene({});
        submit(sink, {}, scene.camera);
        (void)read(sink);
        submit(sink, {}, scene.camera);
        (void)read(sink);
    };
    // First staging creates the model renderer's own resources too; take the baseline after one full cycle.
    sink.activate_scene(sink.stage_scene(scene.refs()));
    settle();
    const RendererDiagnostics base = sink.diagnostics();

    StagedScene staged = sink.stage_scene(scene.refs());
    const RendererDiagnostics during = sink.diagnostics();
    const uint64_t expected = staged.report().texture_bytes + staged.report().vertex_bytes + staged.report().index_bytes;
    CHECK(during.device_memory.allocated_bytes >= base.device_memory.allocated_bytes + expected);  // at least what we uploaded
    CHECK(during.resources.live > base.resources.live);
    CHECK(during.resources.live_bytes > base.resources.live_bytes);

    staged = StagedScene{};  // release the candidate without ever showing it
    settle();
    const RendererDiagnostics after = sink.diagnostics();
    CHECK(after.device_memory.allocated_bytes == base.device_memory.allocated_bytes);  // exact, no leak
    CHECK(after.device_memory.allocation_count == base.device_memory.allocation_count);
    CHECK(after.resources.live == base.resources.live);
    CHECK(after.validation_errors == 0);
}

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

/// A flat 256 x 256 grid of quads (66,049 vertices, 131,072 triangles) in the XY plane, facing +Z, uv spanning [0,1].
gfx::Primitive grid_primitive(uint32_t cells)
{
    gfx::Primitive p;
    const uint32_t n = cells + 1;
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(cells);
            const float v = static_cast<float>(y) / static_cast<float>(cells);
            p.positions.insert(p.positions.end(), {u * 2.0f - 1.0f, v * 2.0f - 1.0f, 0.0f});
            p.normals.insert(p.normals.end(), {0.0f, 0.0f, 1.0f});
            p.uv0.insert(p.uv0.end(), {u, v});
        }
    }
    for (uint32_t y = 0; y < cells; ++y) {
        for (uint32_t x = 0; x < cells; ++x) {
            const uint32_t a = y * n + x, b = a + 1, c = a + n, d = c + 1;
            p.indices.insert(p.indices.end(), {a, b, d, a, d, c});  // counter-clockwise seen from +Z
        }
    }
    p.material = 0;
    return p;
}

} // namespace

// Baselines, not thresholds: this prints what the pipeline costs on this machine so later work (an instance buffer,
// async upload, streaming) can be justified by measurement. The assertions are only sanity bounds. On a software
// rasterizer the numbers describe the CPU path, and the log line says so.
TEST_CASE("Scene measurements: decode, mips, staging and per-instance submit cost")
{
    Scene scene;
    RendererFrameSink sink(make_device("scene_measure"), kW, kH, sink_settings());
    const std::string adapter = sink.diagnostics().adapter;

    // 1) A 1024x1024 texture: PNG decode and mip chain generation (CPU).
    std::vector<uint8_t> pixels(size_t{1024} * 1024 * 4);
    for (uint32_t y = 0; y < 1024; ++y) {
        for (uint32_t x = 0; x < 1024; ++x) {
            uint8_t* px = &pixels[(size_t{y} * 1024 + x) * 4];
            px[0] = static_cast<uint8_t>(x ^ y);
            px[1] = static_cast<uint8_t>(x * 3 + (y >> 2));
            px[2] = static_cast<uint8_t>(y * 5);
            px[3] = 255;
        }
    }
    const std::vector<uint8_t> png = saige::test::make_png(1024, 1024, 8, 6, pixels, size_t{1024} * 4);
    auto t0 = Clock::now();
    const gfx::ImageDecodeResult decoded = gfx::decode_image(png);
    const double decode_ms = ms_since(t0);
    REQUIRE(decoded.ok());
    t0 = Clock::now();
    const gfx::MipChain chain = gfx::generate_mips(*decoded.image, gfx::ColorSpace::Srgb);
    const double mips_ms = ms_since(t0);
    CHECK(chain.levels == 11);

    // 2) Staging a 131k-triangle mesh with that texture (decode + mips + upload + materials), end to end.
    gfx::GltfAsset big = scene.asset;
    big.images[0].bytes = png;
    big.meshes[0].primitives[0] = grid_primitive(256);
    gfx::StageReport report;
    t0 = Clock::now();
    StagedScene staged = sink.stage_scene(std::vector<SceneAssetRef>{{scene.id, &big, "bench"}}, {});
    const double stage_ms = ms_since(t0);
    report = staged.report();
    CHECK(report.primitives == 1);
    CHECK(report.texture_bytes >= 1024u * 1024u * 4u);
    sink.activate_scene(std::move(staged));
    const rhi::DeviceMemoryStats memory = sink.diagnostics().device_memory;

    // 3) Per-instance cost of submit(): the fixture's 24-vertex box, 1 / 100 / 1000 instances (CPU wall time per frame,
    //    which for the offscreen path includes waiting for the frame slot).
    sink.activate_scene(sink.stage_scene(scene.refs()));
    struct Sample {
        int instances;
        double frame_ms;
    };
    std::vector<Sample> samples;
    for (const int count : {1, 100, 1000}) {
        std::vector<RenderInstance> instances;
        for (int i = 0; i < count; ++i) {
            const float gx = static_cast<float>(i % 40) * 1.5f;
            const float gy = static_cast<float>(i / 40) * 1.5f - 10.0f;
            instances.push_back(make_instance(scene.id, translation({gx, gy, 0.0f}), static_cast<uint32_t>(i + 1)));
        }
        for (int warm = 0; warm < 2; ++warm) submit(sink, instances, scene.camera);
        std::vector<double> times;
        for (int i = 0; i < 8; ++i) {
            t0 = Clock::now();
            submit(sink, instances, scene.camera);
            times.push_back(ms_since(t0));
        }
        samples.push_back({count, median(times)});
    }
    (void)read(sink);

    std::printf("MEASURE {\"adapter\":\"%s\",\"software_device\":%s,\"png_1024_decode_ms\":%.2f,\"mips_1024_ms\":%.2f,"
                "\"stage_131k_tris_1024_tex_ms\":%.2f,\"staged_texture_bytes\":%llu,\"staged_vertex_bytes\":%llu,"
                "\"staged_index_bytes\":%llu,\"device_allocated_bytes\":%llu,\"submit_ms\":{\"1\":%.3f,\"100\":%.3f,\"1000\":%.3f},"
                "\"submit_us_per_instance_at_1000\":%.2f}\n",
                adapter.c_str(), sink.diagnostics().software_device ? "true" : "false", decode_ms, mips_ms, stage_ms,
                static_cast<unsigned long long>(report.texture_bytes), static_cast<unsigned long long>(report.vertex_bytes),
                static_cast<unsigned long long>(report.index_bytes), static_cast<unsigned long long>(memory.allocated_bytes),
                samples[0].frame_ms, samples[1].frame_ms, samples[2].frame_ms,
                samples[2].frame_ms);  // ms per 1000 instances is numerically the same as microseconds per instance

    // Sanity only: nothing here may take tens of seconds, and cost must grow with the work.
    CHECK(decode_ms < 30'000.0);
    CHECK(stage_ms < 60'000.0);
    CHECK(samples[2].frame_ms > samples[0].frame_ms);
    CHECK(sink.diagnostics().validation_errors == 0);
}

int main(int argc, char** argv)
{
    std::string reason;
    if (!rhi::vulkan_device_available(&reason)) {
        std::printf("SKIPPED: no usable Vulkan 1.3 device: %s\n", reason.c_str());
        return 77;
    }
    return saige::test::run_all(argc, argv);
}
