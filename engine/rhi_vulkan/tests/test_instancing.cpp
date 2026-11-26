// Instanced model draws on a real (validated) Vulkan device. Instancing changes how many draw calls a frame needs, never the image: a ModelDraw with many instances, drawn with
// the batched path, with the per-instance fallback (instancing off) and as separate ModelDraws must all give the same picture. CI: lavapipe.

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/render/camera.hpp"
#include "vulkan_test_util.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 256;
constexpr uint32_t kH = 144;

struct Result {
    Image image;
    FrameStats stats;
};

struct Options {
    bool instancing = false;
    bool culling = false;
    bool shadows = false;
    bool blend_material = false;
    uint32_t max_instances = 16384;
};

Mat4 rotation_about_z(float a)
{
    Mat4 m = Mat4::identity();
    m.at(0, 0) = std::cos(a);
    m.at(0, 1) = -std::sin(a);
    m.at(1, 0) = std::sin(a);
    m.at(1, 1) = std::cos(a);
    return m;
}

/// The instance matrices of the test scene: a 4 x 3 grid of cubes in front of the camera with different scales and rotations, two of them mirrored.
std::vector<Mat4> scene_instances(bool with_invalid = false)
{
    std::vector<Mat4> m;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            const float x = 6.0f + static_cast<float>(row) * 1.6f;
            const float y = (static_cast<float>(col) - 1.5f) * 1.7f;
            const float z = 0.4f + static_cast<float>(row) * 0.5f;
            const float s = 0.45f + 0.08f * static_cast<float>(col + row);
            const float flip = (row == 1 && (col == 0 || col == 3)) ? -1.0f : 1.0f;  // two mirrored instances
            m.push_back(translation({x, y, z}) * rotation_about_z(0.3f * static_cast<float>(col + 2 * row)) * scale_matrix({s * flip, s, s}));
        }
    }
    if (with_invalid) {
        Mat4 bad = Mat4::identity();
        bad.at(0, 3) = std::numeric_limits<float>::quiet_NaN();
        m.insert(m.begin() + 5, bad);
    }
    return m;
}

Camera test_camera() { return look_at_camera({0.0f, 0.0f, 3.0f}, {8.0f, 0.0f, 0.5f}, {0.0f, 0.0f, 1.0f}, 1.0f); }

/// Renders the scene: either as `instanced` (one ModelDraw with all the matrices) or as `separate` ModelDraws, one per matrix.
Result render_scene(const Options& o, bool instanced, const std::vector<Mat4>& matrices, int frames = 1)
{
    const auto device = make_validated_device("instancing", true);
    RendererSettings settings = deterministic_settings();
    settings.fog_density = 0.0f;
    settings.frustum_culling = o.culling;
    settings.instancing = o.instancing;
    settings.max_instances = o.max_instances;
    settings.alpha_blending = o.blend_material;
    if (o.shadows) {
        settings.shadows.enabled = true;
        settings.shadows.map_size = 512;
    }
    Result r;
    {
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, settings);
        OffscreenTarget target(*device, kW, kH);
        GpuMaterial material;
        if (o.blend_material) {
            MaterialData d;  // a glTF BLEND material: composited in the sorted phase, never batched
            d.base_color[0] = 0.8f;
            d.base_color[1] = 0.5f;
            d.base_color[2] = 0.3f;
            d.base_color[3] = 0.6f;
            d.alpha_blend = true;
            material = renderer.model_renderer().create_material(d, MaterialTextures{}, "cube.blend");
        } else {
            MaterialParams params;
            params.base_color[0] = 0.8f;
            params.base_color[1] = 0.5f;
            params.base_color[2] = 0.3f;
            material = renderer.model_renderer().create_material(params, {}, {}, "cube");
        }
        GpuModel cube = make_cube(renderer.registry(), std::move(material));
        cube.meshes[0][0].bounds_center = {0, 0, 0};
        cube.meshes[0][0].bounds_radius = 0.8660254f;
        // A ground plane so shadows (and depth ordering) have something to land on: drawn as a normal, non-instanced draw in every variant.
        std::vector<ModelDraw> draws;
        if (instanced) {
            ModelDraw d{&cube, Mat4::identity()};
            d.instances = matrices.data();
            d.instance_count = static_cast<uint32_t>(matrices.size());
            draws.push_back(d);
        } else {
            for (const Mat4& m : matrices) draws.push_back({&cube, m});
        }
        for (int f = 0; f < frames; ++f) r.image = target.render(renderer, {}, test_camera(), static_cast<uint64_t>(f), draws, &r.stats);
    }
    CHECK(device->validation_error_count() == 0);
    return r;
}

int max_difference(const Image& a, const Image& b)
{
    int worst = 0;
    for (uint32_t y = 0; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) worst = std::max(worst, max_channel_delta(a.at(x, y), b.at(x, y)));
    }
    return worst;
}

int covered_pixels(const Image& img)
{
    int n = 0;
    const uint8_t* c0 = img.at(0, 0);
    const uint8_t first[3] = {c0[0], c0[1], c0[2]};
    for (uint32_t y = 0; y < img.height; ++y)
        for (uint32_t x = 0; x < img.width; ++x) {
            const uint8_t* c = img.at(x, y);
            n += (c[0] != first[0] || c[1] != first[1] || c[2] != first[2]) ? 1 : 0;
        }
    return n;
}

} // namespace

TEST_CASE("instancing: one batched draw per primitive and mirror state gives the image of separate draws, with far fewer draw calls")
{
    const std::vector<Mat4> m = scene_instances();
    Options off;
    Options on;
    on.instancing = true;
    const Result reference = render_scene(off, false, m);          // twelve separate ModelDraws
    const Result fallback = render_scene(off, true, m);            // one instanced ModelDraw, instancing off: instance by instance
    const Result batched = render_scene(on, true, m);              // one instanced ModelDraw, instancing on
    CHECK(covered_pixels(reference.image) > 500);                  // the scene shows something
    CHECK(max_difference(reference.image, fallback.image) == 0);
    CHECK(max_difference(reference.image, batched.image) == 0);
    CHECK(fallback.stats.model_instanced_draws == 0);
    CHECK(batched.stats.model_instanced_draws == 2);               // the unmirrored group and the mirrored one, one primitive each
    CHECK(batched.stats.model_instances_batched == 12);
    CHECK(batched.stats.model_instances == 12);
    CHECK(reference.stats.model_instances == 12);
    CHECK(batched.stats.draw_calls < reference.stats.draw_calls);  // 2 instead of 12 model draws (plus the same sky and ground)
    CHECK(reference.stats.draw_calls - batched.stats.draw_calls == 10);
}

TEST_CASE("instancing: frustum culling drops the same instances on the batched path, and the image still matches")
{
    std::vector<Mat4> m = scene_instances();
    m.push_back(translation({-30.0f, 0.0f, 0.0f}));  // behind the camera
    m.push_back(translation({6.0f, 90.0f, 0.0f}));   // far off to the side
    Options off, on;
    off.culling = on.culling = true;
    on.instancing = true;
    const Result reference = render_scene(off, false, m);
    const Result batched = render_scene(on, true, m);
    CHECK(max_difference(reference.image, batched.image) == 0);
    CHECK(reference.stats.model_primitives_culled == 2);
    CHECK(batched.stats.model_primitives_culled == 2);
    CHECK(batched.stats.model_instances_batched == 12);  // only the twelve visible ones reach the instance buffer
}

TEST_CASE("instancing: a degenerate instance is skipped and counted, the rest draw as before")
{
    const std::vector<Mat4> clean = scene_instances();
    const std::vector<Mat4> dirty = scene_instances(true);
    Options on;
    on.instancing = true;
    const Result reference = render_scene(on, true, clean);
    const Result with_bad = render_scene(on, true, dirty);
    CHECK(max_difference(reference.image, with_bad.image) == 0);
    CHECK(with_bad.stats.model_instances_skipped == 1);
    CHECK(with_bad.stats.model_instances == 12);
}

TEST_CASE("instancing: with sun shadows on, the shadow pass draws the instances and the image matches the separate draws")
{
    const std::vector<Mat4> m = scene_instances();
    Options off, on;
    off.shadows = on.shadows = true;
    on.instancing = true;
    const Result reference = render_scene(off, false, m);
    const Result batched = render_scene(on, true, m);
    CHECK(max_difference(reference.image, batched.image) == 0);
    CHECK(batched.stats.shadow_draw_calls == reference.stats.shadow_draw_calls);  // the shadow pass is not batched
    CHECK(batched.stats.model_instanced_draws == 2);
}

TEST_CASE("instancing: blended primitives are not batched, and the sorted phase gives the same picture")
{
    const std::vector<Mat4> m = scene_instances();
    Options off, on;
    off.blend_material = on.blend_material = true;
    on.instancing = true;
    const Result reference = render_scene(off, false, m);
    const Result batched = render_scene(on, true, m);
    CHECK(max_difference(reference.image, batched.image) == 0);
    CHECK(batched.stats.model_instanced_draws == 0);
    CHECK(batched.stats.model_blended_primitives == 12);
}

TEST_CASE("instancing: a frame buffer too small for a placement falls back to instance by instance, and says so")
{
    const std::vector<Mat4> m = scene_instances();
    Options off, small;
    small.instancing = true;
    small.max_instances = 4;  // twelve instances need more than that
    const Result reference = render_scene(off, false, m);
    const Result overflow = render_scene(small, true, m);
    CHECK(max_difference(reference.image, overflow.image) == 0);
    CHECK(overflow.stats.model_instance_overflow == 1);
    CHECK(overflow.stats.model_instanced_draws == 0);
    Options exact;
    exact.instancing = true;
    exact.max_instances = 12;  // exactly the batched entries of this scene: it fits
    const Result fits = render_scene(exact, true, m);
    CHECK(fits.stats.model_instance_overflow == 0);
    CHECK(fits.stats.model_instanced_draws == 2);
    CHECK(max_difference(reference.image, fits.image) == 0);
}

TEST_CASE("instancing: frames submitted back to back reuse the instance buffer ring without corrupting each other")
{
    const std::vector<Mat4> m = scene_instances();
    Options on;
    on.instancing = true;
    on.max_instances = 12;  // exactly one frame's entries: a cursor that was not reset each frame would overflow on the second frame
    Options off;
    const Result reference = render_scene(off, false, m, 1);
    const Result later = render_scene(on, true, m, 5);  // the fifth frame, after four earlier ones used the ring slots
    CHECK(max_difference(reference.image, later.image) == 0);
    CHECK(later.stats.model_instanced_draws == 2);  // the stats are per frame, not cumulative
    CHECK(later.stats.model_instance_overflow == 0);
}

TEST_CASE("instancing: invalid use is rejected, and an instanced draw is harmless on a renderer without instancing")
{
    const auto device = make_validated_device("instancing_invalid");
    RendererSettings bad = deterministic_settings();
    bad.instancing = true;
    bad.max_instances = 0;
    // The model renderer is created on first use, which is where the settings are checked.
    bool threw = false;
    try {
        SceneRenderer r(*device, rhi::Format::RGBA8_SRGB, bad);
        (void)r.model_renderer();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    bad.max_instances = (1u << 20) + 1;
    threw = false;
    try {
        SceneRenderer r(*device, rhi::Format::RGBA8_SRGB, bad);
        (void)r.model_renderer();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    RendererSettings ok = deterministic_settings();
    SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, ok);
    OffscreenTarget target(*device, kW, kH);
    CHECK_FALSE(renderer.model_renderer().instancing_enabled());
    GpuModel cube = make_cube(renderer.registry(), renderer.model_renderer().create_material({}, {}, {}, "cube"));
    ModelDraw d{&cube, Mat4::identity()};
    d.instance_count = 3;  // but no matrices
    std::vector<ModelDraw> draws{d};
    threw = false;
    try {
        (void)target.render(renderer, {}, test_camera(), 0, draws);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(device->validation_error_count() == 0);
}
