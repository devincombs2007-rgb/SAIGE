#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/ambient_occlusion.hpp"
#include "saige/rhi_null/null_device.hpp"
#include "saige/render/scene_renderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace saige::render;

namespace {

bool near(float a, float b) { return std::fabs(a - b) <= 1e-5f; }

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

constexpr uint32_t W = 96, H = 64;

AoView make_view()
{
    AoView v;
    v.width = W;
    v.height = H;
    v.near_plane = 0.1f;
    v.tan_half_fov_y = std::tan(0.5f * 60.0f * 3.14159265f / 180.0f);
    return v;
}

/// A scene in view space (camera at the origin looking down -Z, +Y up): a floor at y = -floor_drop and, if `wall_z` > 0, a wall facing the camera at z = -wall_z, forming a concave corner
/// where they meet. The sky shows where neither is hit. `box_*` adds an optional box standing on the floor.
struct Scene {
    float floor_drop = 1.5f;
    float wall_z = 0.0f;
    float pillar_z = 0.0f;       ///< > 0: a wall segment facing the camera at z = -pillar_z, where |x| < pillar_half
    float pillar_half = 0.5f;
};

AoDepthImage render(const AoView& view, const Scene& scene)
{
    AoDepthImage img(view.width, view.height);
    for (uint32_t y = 0; y < view.height; ++y) {
        for (uint32_t x = 0; x < view.width; ++x) {
            static_cast<void>(x);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(view.height);
            const float dy = (1.0f - 2.0f * v) * view.tan_half_fov_y;  // the ray is (dx, dy, -1) * t, so t is the view distance
            float t = std::numeric_limits<float>::infinity();
            if (dy < 0.0f) t = -scene.floor_drop / dy;
            if (scene.wall_z > 0.0f) t = std::min(t, scene.wall_z);
            if (scene.pillar_z > 0.0f) {
                const float dx = (2.0f * ((static_cast<float>(x) + 0.5f) / static_cast<float>(view.width)) - 1.0f) * view.tan_half_fov_y * static_cast<float>(view.width) / static_cast<float>(view.height);
                if (std::fabs(dx * scene.pillar_z) < scene.pillar_half) t = std::min(t, scene.pillar_z);
            }
            img.at(x, y) = std::isfinite(t) ? view.near_plane / t : 0.0f;
        }
    }
    return img;
}

}  // namespace

TEST_CASE("Ambient occlusion settings: ranges are enforced")
{
    AmbientOcclusionSettings s;
    validate_ambient_occlusion(s);
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    for (const float bad : {nan, inf, -inf, 0.0f, 0.005f, 16.5f}) {
        AmbientOcclusionSettings t = s;
        t.radius = bad;
        CHECK(throws_invalid([&] { validate_ambient_occlusion(t); }));
    }
    for (const float bad : {nan, inf, -0.1f, 4.5f}) {
        AmbientOcclusionSettings t = s;
        t.intensity = bad;
        CHECK(throws_invalid([&] { validate_ambient_occlusion(t); }));
    }
    for (const float bad : {nan, inf, -0.1f, 0.6f}) {
        AmbientOcclusionSettings t = s;
        t.bias = bad;
        CHECK(throws_invalid([&] { validate_ambient_occlusion(t); }));
    }
    AmbientOcclusionSettings edge;
    edge.radius = 0.01f;
    edge.intensity = 0.0f;
    edge.bias = 0.0f;
    validate_ambient_occlusion(edge);
    edge.radius = 16.0f;
    edge.intensity = 4.0f;
    edge.bias = 0.5f;
    validate_ambient_occlusion(edge);
}

TEST_CASE("Ambient occlusion: a mismatched or invalid view is refused")
{
    const AoView view = make_view();
    const AoDepthImage depth = render(view, {});
    AmbientOcclusionSettings s;
    AoView wrong = view;
    wrong.width = W + 1;
    CHECK(throws_invalid([&] { static_cast<void>(ao_generate(depth, wrong, s)); }));
    AoView zero = view;
    zero.width = 0;
    CHECK(throws_invalid([&] { static_cast<void>(ao_generate(AoDepthImage{}, zero, s)); }));
    AoView bad_near = view;
    bad_near.near_plane = 0.0f;
    CHECK(throws_invalid([&] { static_cast<void>(ao_generate(depth, bad_near, s)); }));
    AoView bad_fov = view;
    bad_fov.tan_half_fov_y = std::numeric_limits<float>::quiet_NaN();
    CHECK(throws_invalid([&] { static_cast<void>(ao_generate(depth, bad_fov, s)); }));
    AmbientOcclusionSettings bad;
    bad.radius = 0.0f;
    CHECK(throws_invalid([&] { static_cast<void>(ao_generate(depth, view, bad)); }));
    const AoImage ao = ao_generate(depth, view, s);
    AoImage small(W - 1, H);
    CHECK(throws_invalid([&] { static_cast<void>(ao_blur(small, depth, view, s)); }));
    CHECK(ao.width == W);
}

TEST_CASE("Ambient occlusion: a flat floor and the open sky are exactly open")
{
    const AoView view = make_view();
    const AoDepthImage depth = render(view, {});
    const AoImage ao = ao_generate(depth, view, {});
    float lowest = 1.0f;
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const float d = depth.at(x, y);
            if (d <= 0.0f) CHECK(ao.at(x, y) == 1.0f);  // the sky
            else lowest = std::min(lowest, ao.at(x, y));
        }
    }
    CHECK(lowest > 0.99f);  // a plane is never in front of itself: the bias keeps rounding out
}

TEST_CASE("Ambient occlusion: a concave corner is darkened, and the darkening falls off away from it")
{
    const AoView view = make_view();
    Scene corner;
    corner.wall_z = 5.0f;
    const AoDepthImage depth = render(view, corner);
    AmbientOcclusionSettings s;
    const AoImage ao = ao_generate(depth, view, s);
    // The floor meets the wall where t = 5: dy = -1.5 / 5 = -0.3, a row below the horizon. Find it from the depth image: the last floor row before the wall rows.
    uint32_t seam = 0;
    for (uint32_t y = 0; y < H; ++y) {
        if (std::fabs(depth.at(W / 2, y) - view.near_plane / corner.wall_z) < 1e-6f) seam = y;  // the wall's last row (the floor starts after it)
    }
    CHECK(seam > 0);
    // The crease: the last wall row and the first floor rows (0.75 m of floor is only three rows at this distance).
    const float crease = std::min({ao.at(W / 2, seam), ao.at(W / 2, seam + 1), ao.at(W / 2, seam + 2)});
    CHECK(crease < 0.7f);
    CHECK(crease > 0.2f);  // darkened, not black
    // Falling off along the floor away from the wall: no darker farther out, and fully open well beyond the radius.
    for (uint32_t y = seam + 1; y + 1 < H; ++y) CHECK(ao.at(W / 2, y + 1) >= ao.at(W / 2, y) - 0.02f);
    CHECK(ao.at(W / 2, H - 2) > 0.99f);
    CHECK(ao.at(W / 2, seam / 2) > 0.99f);  // the middle of the wall, far from the floor
    // The blur keeps it a crease: still darker than its surroundings, and never brighter than the open surface.
    const AoImage blurred = ao_blur(ao, depth, view, s);
    CHECK(blurred.at(W / 2, seam + 1) < 0.85f);
    for (const float v : blurred.value) CHECK(v >= 0.0f && v <= 1.0f);
}

TEST_CASE("Ambient occlusion: no intensity means no occlusion, and the effect grows with intensity and radius and shrinks with bias")
{
    const AoView view = make_view();
    Scene corner;
    corner.wall_z = 5.0f;
    const AoDepthImage depth = render(view, corner);
    const auto sum_dark = [&](const AmbientOcclusionSettings& s) {
        const AoImage ao = ao_generate(depth, view, s);
        double total = 0.0;
        for (const float v : ao.value) total += static_cast<double>(1.0f - v);
        return total;
    };
    AmbientOcclusionSettings base;
    AmbientOcclusionSettings none = base;
    none.intensity = 0.0f;
    CHECK(sum_dark(none) == 0.0);
    AmbientOcclusionSettings half = base, twice = base;
    half.intensity = 0.5f;
    twice.intensity = 2.0f;
    CHECK(sum_dark(half) > 0.0);
    CHECK(sum_dark(half) < sum_dark(base));
    CHECK(sum_dark(base) < sum_dark(twice));
    AmbientOcclusionSettings wide = base, narrow = base;
    wide.radius = 1.5f;
    narrow.radius = 0.3f;
    CHECK(sum_dark(narrow) < sum_dark(base));
    CHECK(sum_dark(base) < sum_dark(wide));
    AmbientOcclusionSettings biased = base;
    biased.bias = 0.5f;
    CHECK(sum_dark(biased) < sum_dark(base));
    AmbientOcclusionSettings unbiased = base;
    unbiased.bias = 0.0f;
    CHECK(sum_dark(unbiased) > sum_dark(base));
}

TEST_CASE("Ambient occlusion: a convex edge is not darkened, the pillar's base is, and the result is deterministic")
{
    const AoView view = make_view();
    Scene s;
    s.pillar_z = 4.0f;
    s.pillar_half = 0.6f;
    const AoDepthImage depth = render(view, s);
    AmbientOcclusionSettings settings;
    const AoImage ao = ao_generate(depth, view, settings);
    // The pillar's face (above the floor, inside its width) is a flat wall in front of the sky: open everywhere, including along its silhouette against the sky.
    float lowest = 1.0f;
    for (uint32_t y = 0; y < H / 2 - 2; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            if (std::fabs(depth.at(x, y) - view.near_plane / s.pillar_z) < 1e-7f) lowest = std::min(lowest, ao.at(x, y));
        }
    }
    CHECK(lowest > 0.99f);
    // Where the pillar stands on the floor the crease darkens it.
    float base_min = 1.0f;
    for (uint32_t y = H / 2; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) base_min = std::min(base_min, ao.at(x, y));
    }
    CHECK(base_min < 0.8f);
    const AoImage again = ao_generate(depth, view, settings);
    CHECK(again.value == ao.value);
}

TEST_CASE("Ambient occlusion: a radius too small to span a pixel leaves everything open")
{
    const AoView view = make_view();
    Scene corner;
    corner.wall_z = 5.0f;
    const AoDepthImage depth = render(view, corner);
    AmbientOcclusionSettings tiny;
    tiny.radius = 0.01f;  // 0.01 * 55 px / 5 m = 0.1 px
    const AoImage ao = ao_generate(depth, view, tiny);
    for (const float v : ao.value) CHECK(v == 1.0f);
    // A huge radius is clamped to 48 pixels and still produces valid values.
    AmbientOcclusionSettings huge;
    huge.radius = 16.0f;
    huge.intensity = 4.0f;
    const AoImage big = ao_generate(depth, view, huge);
    for (const float v : big.value) CHECK(v >= 0.0f && v <= 1.0f);
}

TEST_CASE("Ambient occlusion blur: it averages over a surface, and does not bleed across a depth edge or from the sky")
{
    const AoView view = make_view();
    AmbientOcclusionSettings s;
    // Two vertical bands at different depths: the left is dark, the right open. Their depths differ by far more than half the radius.
    AoDepthImage depth(W, H);
    AoImage ao(W, H);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            const bool left = x < W / 2;
            depth.at(x, y) = view.near_plane / (left ? 2.0f : 6.0f);
            ao.at(x, y) = left ? 0.2f : 1.0f;
        }
    }
    const AoImage blurred = ao_blur(ao, depth, view, s);
    CHECK(near(blurred.at(W / 2 - 1, H / 2), 0.2f));  // the dark side next to the edge is untouched by the bright side
    CHECK(near(blurred.at(W / 2, H / 2), 1.0f));
    // The same ao over one surface mixes the two across the boundary, and only within the filter's width.
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) depth.at(x, y) = view.near_plane / 3.0f;
    const AoImage mixed = ao_blur(ao, depth, view, s);
    CHECK(mixed.at(W / 2 - 1, H / 2) > 0.2f + 0.05f);
    CHECK(mixed.at(W / 2, H / 2) < 1.0f - 0.05f);
    CHECK(mixed.at(W / 2 - 1, H / 2) < mixed.at(W / 2, H / 2));
    CHECK(near(mixed.at(W / 2 - 1 - ao_blur_radius, H / 2), 0.2f));
    CHECK(near(mixed.at(W / 2 + ao_blur_radius, H / 2), 1.0f));
    // The sky neither receives nor gives occlusion.
    AoDepthImage sky = depth;
    for (uint32_t y = 0; y < H; ++y) sky.at(W / 2, y) = 0.0f;
    const AoImage with_sky = ao_blur(ao, sky, view, s);
    CHECK(with_sky.at(W / 2, H / 2) == 1.0f);  // the sky pixel itself keeps its (open) value
    CHECK(with_sky.at(W / 2 - 1, H / 2) > 0.2f);  // its neighbour still blurs with the surface on the other side...
    AoImage poisoned = ao;
    for (uint32_t y = 0; y < H; ++y) poisoned.at(W / 2, y) = 0.0f;  // ...but whatever the sky pixel holds does not leak into it
    const AoImage clean = ao_blur(poisoned, sky, view, s);
    CHECK(clean.at(W / 2 - 1, H / 2) == with_sky.at(W / 2 - 1, H / 2));
}

TEST_CASE("AmbientOcclusionPass: a failure at any acquisition releases everything acquired before it, and the pass releases all it made")
{
    // Two layouts, three shaders and the generate pipeline in the constructor, then the apply pipeline: seven device objects.
    for (uint32_t failing = 1; failing <= 7; ++failing) {
        saige::rhi::NullDevice device;
        const uint32_t before = device.live_resource_count();
        device.fail_creation_after(failing);
        bool threw = false;
        try {
            AmbientOcclusionPass pass(device);
            pass.set_apply_format(saige::rhi::Format::RGBA16_FLOAT);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        device.fail_creation_after(0);
        device.wait_idle();
        CHECK(device.live_resource_count() == before);
        CHECK(device.validation_error_count() == 0);
    }
    saige::rhi::NullDevice device;
    const uint32_t before = device.live_resource_count();
    {
        AmbientOcclusionPass pass(device);
        pass.set_apply_format(saige::rhi::Format::RGBA16_FLOAT);
        CHECK(device.live_resource_count() == before + 7);
        pass.set_apply_format(saige::rhi::Format::RGBA16_FLOAT);  // already made: a second call adds nothing
        CHECK(device.live_resource_count() == before + 7);
    }
    device.wait_idle();
    CHECK(device.live_resource_count() == before);
}

TEST_CASE("SceneRenderer with ambient occlusion: a failure while the occlusion passes are created leaves nothing behind")
{
    // The tone mapper takes the first five creations; the occlusion passes the next seven. (A failure later, in the constructor's own shaders and pipelines, is the
    // pre-existing constructor-rollback gap that its own PR covers; this feature adds nothing to it.)
    for (uint32_t failing = 6; failing <= 12; ++failing) {
        saige::rhi::NullDevice device;
        const uint32_t before = device.live_resource_count();
        device.fail_creation_after(failing);
        bool threw = false;
        try {
            RendererSettings s;
            s.hdr = true;
            s.ambient_occlusion.enabled = true;
            SceneRenderer renderer(device, saige::rhi::Format::RGBA8_SRGB, s);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        CHECK(threw);
        device.fail_creation_after(0);
        device.wait_idle();
        CHECK(device.live_resource_count() == before);
    }
}

TEST_CASE("Ambient occlusion: the tap radius is clamped to 48 pixels, however large the radius")
{
    // Two fronto-parallel planes: z = 5 on the left, z = 4.5 (nearer, well within the radius) from x = 60 on. A pixel at x = 0 is 60 pixels from the near plane, and with no
    // bias the near plane would occlude it if a tap could reach it. The clamp keeps every tap within 48 pixels, so the pixel stays exactly open.
    const AoView view = make_view();
    AoDepthImage depth(W, H);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) depth.at(x, y) = view.near_plane / (x < 60 ? 5.0f : 4.5f);
    AmbientOcclusionSettings s;
    s.radius = 16.0f;
    s.bias = 0.0f;
    s.intensity = 4.0f;
    const AoImage ao = ao_generate(depth, view, s);
    for (uint32_t y = 4; y < H - 4; ++y) CHECK(ao.at(0, y) == 1.0f);
    // And a pixel 40 pixels from the step does see it (the step is within the clamped reach, which is the point of the test above).
    float dark = 1.0f;
    for (uint32_t y = 4; y < H - 4; ++y) dark = std::min(dark, ao.at(25, y));
    CHECK(dark < 1.0f);
}

TEST_CASE("Ambient occlusion: the taps rotate from pixel to pixel in a 4x4 pattern")
{
    // A vertical step edge: the geometry is identical along every column, so without a per-pixel rotation every row of a column would have the same occlusion. With the
    // ordered pattern the four rows of a 4x4 block differ.
    const AoView view = make_view();
    AoDepthImage depth(W, H);
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x) depth.at(x, y) = view.near_plane / (x < W / 2 ? 5.0f : 4.4f);
    AmbientOcclusionSettings s;
    s.bias = 0.0f;
    const AoImage ao = ao_generate(depth, view, s);
    const uint32_t x = W / 2 - 3;  // a few pixels from the step, inside the radius
    float lo = 1.0f, hi = 0.0f;
    for (uint32_t y = 30; y < 34; ++y) {
        lo = std::min(lo, ao.at(x, y));
        hi = std::max(hi, ao.at(x, y));
    }
    CHECK(hi - lo > 0.005f);
    // And the pattern repeats every four pixels in both directions.
    for (uint32_t y = 30; y < 34; ++y) CHECK(std::fabs(ao.at(x, y) - ao.at(x, y + 4)) < 3e-3f);  // (same geometry, up to perspective: near the middle row it barely changes with the row)
}

TEST_CASE("Ambient occlusion: the occlusion is independent of the scale of the world (the bias is a fraction of the radius)")
{
    const AoView view = make_view();
    Scene small;
    small.wall_z = 5.0f;
    Scene large;
    large.floor_drop = small.floor_drop * 4.0f;
    large.wall_z = small.wall_z * 4.0f;
    AmbientOcclusionSettings a;
    a.radius = 0.75f;
    a.bias = 0.1f;
    AmbientOcclusionSettings b = a;
    b.radius = a.radius * 4.0f;
    const AoImage ao_small = ao_generate(render(view, small), view, a);
    const AoImage ao_large = ao_generate(render(view, large), view, b);
    float worst = 0.0f;
    float darkest = 1.0f;
    for (std::size_t i = 0; i < ao_small.value.size(); ++i) {
        worst = std::max(worst, std::fabs(ao_small.value[i] - ao_large.value[i]));
        darkest = std::min(darkest, ao_small.value[i]);
    }
    CHECK(darkest < 0.7f);  // not vacuous: the corner is occluded
    CHECK(worst < 2e-3f);
}

TEST_CASE("Ambient occlusion blur: the weights fall off with distance and with depth difference")
{
    const AoView view = make_view();
    AmbientOcclusionSettings s;  // radius 0.75: depth differences count for nothing beyond 0.375 m
    // One occluded pixel in an open field, all at one depth: the result at the pixel and beside it is exactly 1 - w / sum(w) for the documented weights 1 / (1 + dx^2 + dy^2).
    AoDepthImage depth(W, H);
    for (float& d : depth.depth) d = view.near_plane / 3.0f;
    AoImage ao(W, H);
    ao.at(40, 30) = 0.0f;
    const AoImage out = ao_blur(ao, depth, view, s);
    float total = 0.0f;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) total += 1.0f / (1.0f + static_cast<float>(dx * dx + dy * dy));
    CHECK(near(out.at(40, 30), 1.0f - 1.0f / total));
    CHECK(near(out.at(41, 30), 1.0f - 0.5f / total));
    CHECK(near(out.at(41, 31), 1.0f - (1.0f / 3.0f) / total));
    CHECK(near(out.at(42, 32), 1.0f - (1.0f / 9.0f) / total));
    CHECK(out.at(43, 30) == 1.0f);  // beyond the filter
    // A depth step of 0.5 m is more than 0.5 * radius: it blocks the exchange completely. A step of 0.1 m lets part through, less than no step.
    const auto across = [&](float far_z) {
        AoDepthImage d2(W, H);
        AoImage v2(W, H);
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x) {
                const bool left = x < W / 2;
                d2.at(x, y) = view.near_plane / (left ? 3.0f : far_z);
                v2.at(x, y) = left ? 0.2f : 1.0f;
            }
        return ao_blur(v2, d2, view, s).at(W / 2 - 1, 30);
    };
    CHECK(near(across(3.5f), 0.2f));
    const float none = across(3.0f), small_step = across(3.1f);
    CHECK(none > small_step);
    CHECK(small_step > 0.2f + 1e-3f);
}
