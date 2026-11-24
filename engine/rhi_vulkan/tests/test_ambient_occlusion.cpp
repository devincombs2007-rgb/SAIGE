// Ambient occlusion on a real (validated, synchronization-checked) Vulkan device, through the scene renderer: a cube standing on the ground, rendered with and without the
// pass. The GPU's per-pixel ratio (occluded / unoccluded linear color) is compared with the CPU reference (`ao_generate` then `ao_blur`) computed from the same scene's depth,
// which the test rebuilds analytically (ground plane, axis-aligned cube, same camera). Rasterization decides silhouette pixels slightly differently from an analytic ray, so the
// comparison is aggregate (mean and tail) rather than texel exact. Software Vulkan (lavapipe in CI): correctness evidence only, no physical-GPU claim.

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/render/ambient_occlusion.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 160;
constexpr uint32_t kH = 120;

struct SceneSpec {
    Float3 eye{-4.0f, -3.0f, 2.5f};
    Float3 target{0.0f, 0.0f, 0.4f};
    Float3 cube{0.0f, 0.0f, 0.0f};  // the position of the cube's base center (the renderer's convention); unit edge, yaw 0
};

RendererSettings settings(bool ao)
{
    RendererSettings s = deterministic_settings();
    s.hdr = true;
    s.tone_map.op = ToneMapOperator::Linear;
    s.ambient_occlusion.enabled = ao;
    return s;
}

std::vector<SceneInstance> instances(const SceneSpec& spec)
{
    std::vector<SceneInstance> cubes(1);
    cubes[0].position = spec.cube;
    cubes[0].color = {0.6f, 0.45f, 0.3f};
    return cubes;
}

/// The depth the scene renderer produces, rebuilt analytically: the ground plane z = 0 (|x - target.x|, |y - target.y| < ground_half_size) and the cube (its base center at
/// `spec.cube`, so it spans z in [cube.z, cube.z + 1]).
AoDepthImage analytic_depth(const SceneSpec& spec, const Camera& camera, float ground_half)
{
    AoDepthImage out(kW, kH);
    const float aspect = static_cast<float>(kW) / static_cast<float>(kH);
    const Float3 fwd = camera_forward(camera);
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const float ndc_x = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(kW) - 1.0f;
            const float ndc_y = 1.0f - 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(kH);
            const Ray ray = pixel_ray(camera, aspect, ndc_x, ndc_y);
            float t = std::numeric_limits<float>::infinity();
            if (ray.direction.z < 0.0f) {
                const float tg = -ray.origin.z / ray.direction.z;
                const Float3 p = ray.origin + ray.direction * tg;
                if (std::fabs(p.x - spec.target.x) < ground_half && std::fabs(p.y - spec.target.y) < ground_half) t = tg;
            }
            // The cube: slab test against [c - 0.5, c + 0.5]^3.
            float t0 = 0.0f, t1 = std::numeric_limits<float>::infinity();
            const float o[3] = {ray.origin.x, ray.origin.y, ray.origin.z}, d[3] = {ray.direction.x, ray.direction.y, ray.direction.z}, c[3] = {spec.cube.x, spec.cube.y, spec.cube.z + 0.5f};
            bool hit = true;
            for (int i = 0; i < 3; ++i) {
                if (std::fabs(d[i]) < 1e-12f) {
                    if (o[i] < c[i] - 0.5f || o[i] > c[i] + 0.5f) hit = false;
                    continue;
                }
                float a = (c[i] - 0.5f - o[i]) / d[i], b = (c[i] + 0.5f - o[i]) / d[i];
                if (a > b) std::swap(a, b);
                t0 = std::max(t0, a);
                t1 = std::min(t1, b);
            }
            if (hit && t0 <= t1) t = std::min(t, t0);
            if (!std::isfinite(t)) continue;  // the sky
            out.at(x, y) = camera.near_plane / (t * dot(ray.direction, fwd));  // reverse-Z: near / view distance
        }
    }
    return out;
}

struct Frames {
    Image off, on, on_again;
    FrameStats stats;
};

Frames render_frames(const SceneSpec& spec, const AmbientOcclusionSettings& ao_settings)
{
    Frames f;
    const Camera camera = look_at_camera(spec.eye, spec.target);
    {
        const auto device = make_validated_device("ao_off", /*synchronization=*/true);
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, settings(false));
        OffscreenTarget target(*device, kW, kH);
        f.off = target.render(renderer, instances(spec), camera, 0, {});
        CHECK(device->validation_error_count() == 0);
    }
    {
        const auto device = make_validated_device("ao_on", /*synchronization=*/true);
        RendererSettings s = settings(true);
        s.ambient_occlusion = ao_settings;
        s.ambient_occlusion.enabled = true;
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, s);
        OffscreenTarget target(*device, kW, kH);
        f.on = target.render(renderer, instances(spec), camera, 0, {}, &f.stats);
        f.on_again = target.render(renderer, instances(spec), camera, 0, {});  // the second frame starts from the first one's resource states
        CHECK(device->validation_error_count() == 0);
    }
    return f;
}

}  // namespace

TEST_CASE("Ambient occlusion on the GPU: the pass matches the CPU reference, darkens the cube's base, leaves the sky alone, and is deterministic across frames")
{
    const SceneSpec spec;
    AmbientOcclusionSettings ao;
    ao.radius = 0.6f;
    ao.intensity = 1.5f;
    ao.bias = 0.1f;  // every parameter is a real input: the push constants reach the shader
    const Frames f = render_frames(spec, ao);
    CHECK(f.stats.ao_draw_calls == 2);
    CHECK(f.on.rgba == f.on_again.rgba);

    RendererSettings defaults;
    const Camera camera = look_at_camera(spec.eye, spec.target);
    AoView view;
    view.width = kW;
    view.height = kH;
    view.near_plane = camera.near_plane;
    view.tan_half_fov_y = std::tan(0.5f * camera.fov_y_radians);
    const AoDepthImage depth = analytic_depth(spec, camera, defaults.ground_half_size);
    AmbientOcclusionSettings used = ao;
    used.enabled = true;
    const AoImage reference = ao_blur(ao_generate(depth, view, used), depth, view, used);

    double abs_error = 0.0;
    float worst = 0.0f;
    uint32_t over2 = 0, over1 = 0;
    uint32_t compared = 0, big = 0, darkened = 0, ref_darkened = 0, sky_changed = 0, brightened = 0;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const uint8_t* off = f.off.at(x, y);
            const uint8_t* on = f.on.at(x, y);
            if (depth.at(x, y) <= 0.0f) {
                sky_changed += max_channel_delta(off, on) > 0 ? 1u : 0u;
                continue;
            }
            // The ratio of linear colors, from a channel bright enough that 8-bit quantization is a small error.
            const float lo = srgb_to_linear(off[0]), ln = srgb_to_linear(on[0]);
            if (lo < 0.25f) continue;
            const float ratio = std::min(ln / lo, 1.0f + 1e-3f);
            const float want = reference.at(x, y);
            const float err = std::fabs(ratio - want);
            abs_error += static_cast<double>(err);
            worst = std::max(worst, err);
            over2 += err > 0.02f ? 1u : 0u;
            over1 += err > 0.01f ? 1u : 0u;
            ++compared;
            big += err > 0.08f ? 1u : 0u;
            darkened += ratio < 0.9f ? 1u : 0u;
            ref_darkened += want < 0.9f ? 1u : 0u;
            brightened += ln > lo * 1.01f + 0.01f ? 1u : 0u;
        }
    }
    const double mean = abs_error / std::max(compared, 1u);
    std::printf("  worst %.4f, >0.02: %u, >0.01: %u\n", static_cast<double>(worst), over2, over1);
    std::printf("  %u pixels compared: mean |gpu - reference| = %.4f, %u beyond 0.08; darkened by the pass %u (reference %u); sky pixels changed %u\n", compared, mean, big, darkened, ref_darkened,
                sky_changed);
    CHECK(compared > 5000);
    CHECK(mean < 0.002);
    // Pattern, blur-weight and tap mistakes in the shader show up as 0.07 to 0.14 worst-case errors and hundreds of pixels beyond 0.02; float and rasterization noise is 0.016 and none.
    CHECK(worst < 0.03f);
    CHECK(over2 == 0);
    CHECK(over1 <= 60);
    CHECK(big <= compared / 500);        // (any silhouette pixel where rasterization and the analytic ray disagree about which surface is there)
    CHECK(ref_darkened >= 60);           // the cube's base really is occluded in the reference (not a vacuous comparison)
    CHECK(darkened >= ref_darkened / 2);  // and on the GPU
    CHECK(darkened <= ref_darkened * 2);
    CHECK(sky_changed == 0);
    CHECK(brightened == 0);  // occlusion only ever darkens
}

TEST_CASE("Ambient occlusion on the GPU: zero intensity changes nothing, and a larger radius darkens at least as many pixels")
{
    const SceneSpec spec;
    AmbientOcclusionSettings none;
    none.intensity = 0.0f;
    const Frames f = render_frames(spec, none);
    CHECK(f.on.rgba == f.off.rgba);

    const auto darkened_by = [&](float radius) {
        AmbientOcclusionSettings a;
        a.radius = radius;
        a.intensity = 1.5f;
        const Frames g = render_frames(spec, a);
        uint32_t n = 0;
        for (uint32_t y = 0; y < kH; ++y)
            for (uint32_t x = 0; x < kW; ++x) n += max_channel_delta(g.off.at(x, y), g.on.at(x, y)) > 2 ? 1u : 0u;
        return n;
    };
    const uint32_t small = darkened_by(0.3f), large = darkened_by(1.2f);
    std::printf("  pixels darkened: radius 0.3 -> %u, radius 1.2 -> %u\n", small, large);
    CHECK(small > 0);
    CHECK(large > small);
}
