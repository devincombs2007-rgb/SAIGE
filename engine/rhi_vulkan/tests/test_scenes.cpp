// Scene-level Vulkan tests: camera correctness, depth/occlusion invariants and golden-image regression, all rendered
// offscreen on whatever Vulkan device is present (CI: lavapipe, a CPU driver). Validation errors fail each test.
//
// Goldens live in tests/goldens (160x90 PPM). To create or intentionally change one:
//   SAIGE_UPDATE_GOLDENS=1 ./saige_rhi_vulkan_tests golden
// then LOOK at the images and review the git diff.

#include "saige/gfx_test/saige_test.hpp"

#include "vulkan_test_util.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

#ifndef SAIGE_GOLDEN_DIR
#    error "SAIGE_GOLDEN_DIR must point at engine/rhi_vulkan/tests/goldens"
#endif

namespace {

constexpr const char* golden_dir = SAIGE_GOLDEN_DIR;

SceneInstance cube(Float3 position, Float3 color, float scale = 1.0f, float yaw = 0.0f)
{
    SceneInstance s;
    s.position = position;
    s.color = color;
    s.scale = scale;
    s.yaw_offset = yaw;
    return s;
}

constexpr Float3 red{0.9f, 0.08f, 0.05f};
constexpr Float3 blue{0.05f, 0.2f, 0.95f};
constexpr Float3 green{0.1f, 0.8f, 0.15f};
constexpr Float3 yellow{0.9f, 0.8f, 0.05f};

/// Camera at the origin of the scenes below, looking along +X: near objects are at small x, far at large x.
Camera front_camera()
{
    return look_at_camera({-1.0f, 0.0f, 1.2f}, {8.0f, 0.0f, 0.8f});
}

/// Three comparable cubes that genuinely interpenetrate, so each shows faces inside and outside the others.
std::vector<SceneInstance> intersecting_scene()
{
    return {cube({0.0f, 0.0f, 0.0f}, red, 2.4f), cube({1.3f, 0.9f, 0.3f}, blue, 2.4f, 0.6f),
            cube({0.3f, -1.3f, 0.1f}, green, 2.0f, 1.1f)};
}

Camera high_camera()
{
    return look_at_camera({-7.0f, -6.0f, 7.0f}, {1.0f, 0.5f, 0.0f}, {0, 0, 1}, 55.0f * pi / 180.0f, 0.1f);
}

} // namespace

TEST_CASE("Vulkan scenes: world points project onto the pixels where their cubes are rendered")
{
    const auto device = make_validated_device("scenes_projection");
    {
        OffscreenTarget target(*device, 320, 180);
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, deterministic_settings());
        const Camera camera = look_at_camera({-8.0f, -5.0f, 3.0f}, {2.0f, 1.0f, 0.5f});
        const float aspect = 320.0f / 180.0f;
        const Mat4 vp = view_projection_matrix(camera, aspect);
        const Image empty = target.render(renderer, {}, camera);

        for (const Float3 base : {Float3{0, 0, 0}, Float3{2, 1, 0}, Float3{4, -2, 0}, Float3{-1, 3, 0}, Float3{3, 3, 0.5f}}) {
            const float edge = 0.5f;
            const Image with = target.render(renderer, {cube(base, red, edge)}, camera);
            const Mask m = changed_pixels(with, empty);
            const Float3 center = base + Float3{0, 0, 0.5f * edge};  // the instance position is the cube's base
            const Projected p = project_point(vp, center);
            REQUIRE(p.in_front);
            const PixelPos expected = ndc_to_pixel(p.ndc_x, p.ndc_y, 320.0f, 180.0f);
            std::printf("  base (%.1f,%.1f,%.1f): %u px, centroid (%.1f, %.1f) vs projected (%.1f, %.1f)\n",
                        static_cast<double>(base.x), static_cast<double>(base.y), static_cast<double>(base.z), m.count,
                        m.cx, m.cy, static_cast<double>(expected.x), static_cast<double>(expected.y));
            CHECK(m.count > 20);
            CHECK(std::fabs(m.cx - static_cast<double>(expected.x)) < 2.0);
            CHECK(std::fabs(m.cy - static_cast<double>(expected.y)) < 2.0);
        }
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan scenes: a near cube hides a far cube whatever the draw order")
{
    const auto device = make_validated_device("scenes_occlusion");
    {
        OffscreenTarget target(*device, 320, 180);
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, deterministic_settings());
        const Camera camera = front_camera();
        const SceneInstance near_blue = cube({3.0f, -0.5f, 0.0f}, blue, 1.0f);   // small, close, left of center line
        const SceneInstance far_red = cube({9.0f, -0.5f, 0.0f}, red, 3.0f);      // big, far, directly behind it

        const Image far_first = target.render(renderer, {far_red, near_blue}, camera);
        const Image near_first = target.render(renderer, {near_blue, far_red}, camera);
        CHECK(far_first.rgba == near_first.rgba);  // depth testing makes the result independent of submission order

        // Attribute pixels to each cube by rendering it alone, then see how much of each survives in the combination.
        const Image empty = target.render(renderer, {}, camera);
        const Image only_near = target.render(renderer, {near_blue}, camera);
        const Image only_far = target.render(renderer, {far_red}, camera);
        const uint32_t near_silhouette = silhouette_pixels(only_near, empty);
        const uint32_t far_silhouette = silhouette_pixels(only_far, empty);
        const uint32_t near_visible = visible_pixels(far_first, only_near, empty);
        const uint32_t far_visible = visible_pixels(far_first, only_far, empty);
        std::printf("  near cube: %u of %u px visible; far cube: %u of %u px visible\n", near_visible, near_silhouette,
                    far_visible, far_silhouette);
        CHECK(near_silhouette > 300);
        CHECK(near_visible == near_silhouette);        // nothing is in front of the near cube: all of it shows
        CHECK(far_visible > 400);                      // the far cube still shows around the near one...
        CHECK(far_visible + 150 < far_silhouette);     // ...but a substantial part of it is hidden behind the near cube
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan scenes: intersecting cubes show a correct intersection in every draw order")
{
    const auto device = make_validated_device("scenes_intersection");
    {
        OffscreenTarget target(*device, 320, 180);
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, deterministic_settings());
        const Camera camera = high_camera();
        const std::vector<SceneInstance> base = intersecting_scene();
        const Image reference = target.render(renderer, base, camera);
        if (const char* capture = std::getenv("SAIGE_CAPTURE_DIR")) {  // only when asked: never litter the cwd
            write_ppm(std::string(capture) + "/intersecting_cubes.ppm", reference);
        }

        // All six permutations of three instances must give a byte-identical image.
        std::vector<int> order{0, 1, 2};
        int permutations = 0;
        do {
            std::vector<SceneInstance> shuffled;
            for (int i : order) {
                shuffled.push_back(base[static_cast<size_t>(i)]);
            }
            const Image img = target.render(renderer, shuffled, camera);
            CHECK(img.rgba == reference.rgba);
            ++permutations;
        } while (std::next_permutation(order.begin(), order.end()));
        CHECK(permutations == 6);

        // No cube is wholly swallowed by the others: each keeps a substantial part of its own silhouette visible, and
        // each is partly hidden by another (so the cubes really do intersect rather than sit apart).
        const Image empty = target.render(renderer, {}, camera);
        for (size_t i = 0; i < base.size(); ++i) {
            const Image alone = target.render(renderer, {base[i]}, camera);
            const uint32_t silhouette = silhouette_pixels(alone, empty);
            const uint32_t visible = visible_pixels(reference, alone, empty);
            std::printf("  cube %zu: %u of %u px visible\n", i, visible, silhouette);
            CHECK(visible > 300);
            CHECK(visible < silhouette);
        }
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan scenes: rendering is deterministic (identical input gives byte-identical output)")
{
    const auto device = make_validated_device("scenes_determinism");
    {
        OffscreenTarget target(*device, 160, 90);
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB);  // default settings: spinning cubes, driven by tick
        const Camera camera = high_camera();
        const std::vector<SceneInstance> scene{cube({0, 0, 0}, red), cube({3, 1, 0}, blue, 1.5f), cube({-2, 2, 0}, green)};
        const Image a = target.render(renderer, scene, camera, 37);
        const Image b = target.render(renderer, scene, camera, 37);
        CHECK(a.rgba == b.rgba);
        const Image later = target.render(renderer, scene, camera, 38);
        CHECK(later.rgba != a.rgba);  // and the animation really does depend on the tick
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

namespace {

void check_scene_golden(const char* name, const std::vector<SceneInstance>& scene, const Camera& camera)
{
    const auto device = make_validated_device("scenes_golden");
    Image actual;
    {
        OffscreenTarget target(*device, 160, 90);
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, deterministic_settings());
        actual = target.render(renderer, scene, camera);
    }
    device->wait_idle();
    const GoldenOutcome outcome = check_golden(golden_dir, name, actual);
    if (!outcome.message.empty()) {
        std::printf("  %s\n", outcome.message.c_str());
    }
    CHECK(outcome.ok());
    CHECK(device->validation_error_count() == 0);
}

} // namespace

TEST_CASE("Vulkan golden: empty world (sky, fog and ground only)")
{
    check_scene_golden("empty_world", {}, high_camera());
}

TEST_CASE("Vulkan golden: near cube occluding a far cube")
{
    check_scene_golden("occlusion_near_far",
                       {cube({9.0f, -0.5f, 0.0f}, red, 3.0f), cube({3.0f, -0.5f, 0.0f}, blue, 1.0f)}, front_camera());
}

TEST_CASE("Vulkan golden: three interpenetrating cubes")
{
    check_scene_golden("intersecting_cubes", intersecting_scene(), high_camera());
}

TEST_CASE("Vulkan golden: a grid of cubes in perspective")
{
    std::vector<SceneInstance> grid;
    const Float3 colors[4] = {red, blue, green, yellow};
    for (int ix = 0; ix < 5; ++ix) {
        for (int iy = 0; iy < 3; ++iy) {
            grid.push_back(cube({static_cast<float>(ix) * 3.0f, static_cast<float>(iy - 1) * 3.0f, 0.0f},
                                colors[(ix + iy) % 4], 0.6f + 0.2f * static_cast<float>((ix * 3 + iy) % 4),
                                0.3f * static_cast<float>(ix + iy)));
        }
    }
    check_scene_golden("grid_perspective", grid, look_at_camera({-6.0f, 0.0f, 3.5f}, {5.0f, 0.0f, 0.0f}));
}
