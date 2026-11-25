// Frustum culling on a real (validated) Vulkan device: culling changes the work, never the image. Many cubes on a ring around the camera, most of
// them outside the view or behind it, are rendered with culling on and off; the images must be identical and the culled run must record fewer
// draws. A second scene puts cubes straddling every screen edge, which a wrong (too aggressive) test would clip. CI: lavapipe.

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/render/camera.hpp"
#include "vulkan_test_util.hpp"

#include <cmath>
#include <cstdint>
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

Result render_scene(bool culling, const std::vector<Float3>& positions, const Camera& camera, float scale)
{
    const auto device = make_validated_device("frustum_culling");
    RendererSettings settings = deterministic_settings();
    settings.fog_density = 0.0f;
    settings.frustum_culling = culling;
    SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, settings);
    OffscreenTarget target(*device, kW, kH);
    MaterialParams params;
    params.base_color[0] = 0.8f;
    params.base_color[1] = 0.5f;
    params.base_color[2] = 0.3f;
    GpuModel cube = make_cube(renderer.registry(), renderer.model_renderer().create_material(params, {}, {}, "cube"));
    cube.meshes[0][0].bounds_center = {0, 0, 0};
    cube.meshes[0][0].bounds_radius = 0.8660254f;  // half the cube's diagonal
    std::vector<ModelDraw> draws;
    for (const Float3 p : positions) draws.push_back({&cube, translation(p) * scale_matrix({scale, scale, scale})});
    Result r;
    r.image = target.render(renderer, {}, camera, 0, draws, &r.stats);
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

} // namespace

TEST_CASE("Frustum culling: culling changes the work and never the image (ring of cubes around the camera)")
{
    const Camera camera = look_at_camera({0, 0, 1.5f}, {10, 0, 1.0f});
    std::vector<Float3> positions;
    for (int i = 0; i < 48; ++i) {  // a ring at 9 m: only the arc in front of the camera is in view
        const float a = static_cast<float>(i) * 6.2831853f / 48.0f;
        positions.push_back({9.0f * std::cos(a), 9.0f * std::sin(a), 0.5f});
    }
    for (int i = 0; i < 12; ++i) positions.push_back({14.0f + static_cast<float>(i) * 0.1f, -3.0f + static_cast<float>(i) * 0.5f, 0.5f});  // a diagonal run in view
    const Result with = render_scene(true, positions, camera, 1.0f), without = render_scene(false, positions, camera, 1.0f);
    CHECK(max_difference(with.image, without.image) == 0);
    CHECK(with.stats.model_primitives_culled >= 30);
    CHECK(without.stats.model_primitives_culled == 0);
    CHECK(with.stats.draw_calls + with.stats.model_primitives_culled == without.stats.draw_calls);
    CHECK(with.stats.model_instances == without.stats.model_instances);
}

TEST_CASE("Frustum culling: cubes straddling every screen edge and the near plane are not clipped away")
{
    const Camera camera = look_at_camera({0, 0, 1.5f}, {10, 0, 1.5f});
    // At 6 m the half extents of the view are about 3.46 m (height) by 6.2 m (width at 16:9). Cubes centered a hair outside each edge still reach in.
    std::vector<Float3> positions;
    for (const float dx : {6.0f}) {
        positions.push_back({dx, 6.2f + 0.6f, 1.5f});   // right
        positions.push_back({dx, -6.2f - 0.6f, 1.5f});  // left
        positions.push_back({dx, 0.0f, 1.5f + 3.46f + 0.6f});  // top
        positions.push_back({dx, 0.0f, 1.5f - 3.46f - 0.6f});  // bottom
    }
    positions.push_back({0.4f, 0.0f, 1.5f});  // straddling the near plane and the camera
    positions.push_back({30.0f, 0.0f, 1.5f});  // far away, small but visible
    for (const float scale : {1.0f, 2.0f}) {
        const Result with = render_scene(true, positions, camera, scale), without = render_scene(false, positions, camera, scale);
        CHECK(max_difference(with.image, without.image) == 0);
    }
}
