#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/shadow_cascades.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace saige::render;

namespace {

CascadeCamera camera_at(Float3 position, Float3 forward)
{
    CascadeCamera c;
    c.position = position;
    c.forward = normalize(forward);
    return c;
}

Float3 project(const Mat4& m, Float3 p)
{
    const Float4 r = m * Float4{p.x, p.y, p.z, 1.0f};
    return {r.x / r.w, r.y / r.w, r.z / r.w};
}

/// A point of the camera's view-space slice [slice_near, slice_far]: depth along forward and normalized offsets (-1..1) in x and y.
Float3 slice_point(const CascadeCamera& cam, float depth, float u, float v)
{
    const Float3 world_up{0.0f, 0.0f, 1.0f};
    Float3 right = cross(cam.forward, world_up);
    right = length(right) > 1e-4f ? normalize(right) : Float3{1.0f, 0.0f, 0.0f};
    const Float3 up = cross(right, cam.forward);
    const float half_h = depth * std::tan(cam.fov_y_radians * 0.5f);
    return cam.position + cam.forward * depth + right * (u * half_h * cam.aspect) + up * (v * half_h);
}

float frac(float x) { return x - std::floor(x); }

} // namespace

TEST_CASE("Cascades: splits are monotone, hit the exact ends, and blend uniform and logarithmic")
{
    const auto uniform = cascade_splits(1.0f, 101.0f, 4, 0.0f);
    CHECK(uniform[0] == 1.0f);
    CHECK(uniform[4] == 101.0f);
    CHECK(std::fabs(uniform[1] - 26.0f) < 1e-4f);
    CHECK(std::fabs(uniform[2] - 51.0f) < 1e-4f);
    const auto logarithmic = cascade_splits(1.0f, 81.0f, 4, 1.0f);
    for (int i = 1; i <= 4; ++i) {
        CHECK(std::fabs(logarithmic[static_cast<size_t>(i)] / logarithmic[static_cast<size_t>(i) - 1] - 3.0f) < 1e-3f);  // 81^(1/4) = 3
    }
    for (const float lambda : {0.0f, 0.3f, 0.75f, 1.0f}) {
        for (const uint32_t count : {1u, 2u, 3u, 4u}) {
            const auto s = cascade_splits(0.1f, 150.0f, count, lambda);
            CHECK(s[0] == 0.1f);
            CHECK(s[count] == 150.0f);
            for (uint32_t i = 1; i <= count; ++i) CHECK(s[i] > s[i - 1]);
            for (uint32_t i = count + 1; i <= kMaxShadowCascades; ++i) CHECK(s[i] == 0.0f);
        }
    }
    // Invalid input yields nothing.
    for (const auto& s : {cascade_splits(1.0f, 1.0f, 4, 0.5f), cascade_splits(-1.0f, 10.0f, 4, 0.5f), cascade_splits(1.0f, 10.0f, 0, 0.5f),
                          cascade_splits(1.0f, 10.0f, 5, 0.5f), cascade_splits(std::nanf(""), 10.0f, 4, 0.5f)}) {
        CHECK(s[0] == 0.0f);
        CHECK(s[1] == 0.0f);
    }
}

TEST_CASE("Cascades: the slice's bounding sphere reaches every corner and no corner is farther than the radius")
{
    for (const float aspect : {1.0f, 16.0f / 9.0f, 2.4f}) {
        for (const float fov : {0.6f, 1.0471976f, 1.8f}) {
            for (const auto& range : {std::pair{0.1f, 5.0f}, std::pair{5.0f, 40.0f}, std::pair{40.0f, 150.0f}, std::pair{0.1f, 150.0f}}) {
                const SliceSphere s = slice_bounding_sphere(range.first, range.second, fov, aspect);
                CascadeCamera cam;
                cam.position = {};
                cam.forward = {1.0f, 0.0f, 0.0f};
                cam.fov_y_radians = fov;
                cam.aspect = aspect;
                float farthest = 0.0f;
                for (const float depth : {range.first, range.second}) {
                    for (const float u : {-1.0f, 1.0f}) {
                        for (const float v : {-1.0f, 1.0f}) {
                            farthest = std::max(farthest, length(slice_point(cam, depth, u, v) - Float3{s.axis_distance, 0.0f, 0.0f}));
                        }
                    }
                }
                CHECK(std::fabs(farthest - s.radius) < 1e-3f * s.radius);  // the sphere passes through the farthest corners and contains all
            }
        }
    }
}

TEST_CASE("Cascades: every point of every slice lands inside its cascade's light-space volume")
{
    const CascadeSettings settings;
    for (const Float3 sun : {normalize(Float3{-0.40f, -0.55f, 0.73f}), Float3{0.0f, 0.0f, 1.0f}, normalize(Float3{1.0f, 0.2f, 0.05f})}) {
        const CascadeCamera cam = camera_at({3.0f, -7.0f, 2.0f}, {0.6f, 0.7f, -0.1f});
        const ShadowCascades set = fit_shadow_cascades(cam, sun, settings);
        REQUIRE(set.count == 4);
        size_t checked = 0, outside = 0;
        for (uint32_t i = 0; i < set.count; ++i) {
            const ShadowCascade& c = set.cascades[i];
            for (int a = 0; a <= 8; ++a) {
                for (int b = 0; b <= 8; ++b) {
                    for (int d = 0; d <= 4; ++d) {
                        const float depth = c.split_near + (c.split_far - c.split_near) * static_cast<float>(d) / 4.0f;
                        const Float3 p = project(c.view_proj, slice_point(cam, depth, static_cast<float>(a) / 4.0f - 1.0f, static_cast<float>(b) / 4.0f - 1.0f));
                        ++checked;
                        const float eps = 1e-3f;
                        outside += (std::fabs(p.x) > 1.0f + eps || std::fabs(p.y) > 1.0f + eps || p.z < -eps || p.z > 1.0f + eps) ? 1u : 0u;
                    }
                }
            }
        }
        CHECK(checked > 1000);
        CHECK(outside == 0);
    }
}

TEST_CASE("Cascades: with stabilization a world point keeps its fractional texel position as the camera moves and turns")
{
    CascadeSettings settings;
    settings.resolution = 1024;
    const Float3 sun = normalize(Float3{-0.40f, -0.55f, 0.73f});
    const Float3 point{12.3f, -4.7f, 1.9f};
    const auto texel_position = [&](const CascadeCamera& cam, uint32_t cascade, float resolution) {
        const ShadowCascades set = fit_shadow_cascades(cam, sun, settings);
        const Float3 p = project(set.cascades[cascade].view_proj, point);
        return std::pair{frac((p.x * 0.5f + 0.5f) * resolution), frac((p.y * 0.5f + 0.5f) * resolution)};
    };
    for (uint32_t cascade = 0; cascade < 4; ++cascade) {
        const auto reference = texel_position(camera_at({0.0f, 0.0f, 2.0f}, {1.0f, 0.0f, 0.0f}), cascade, 1024.0f);
        float worst = 0.0f;
        for (int i = 1; i <= 40; ++i) {
            const float t = static_cast<float>(i);
            // Move by amounts that are not texel multiples and turn by arbitrary angles.
            const CascadeCamera cam = camera_at({0.137f * t, -0.211f * t, 2.0f + 0.013f * t}, {std::cos(0.173f * t), std::sin(0.173f * t), -0.05f * std::sin(t)});
            const auto now = texel_position(cam, cascade, 1024.0f);
            const auto wrap = [](float d) { return std::min(std::fabs(d), 1.0f - std::fabs(d)); };  // a fraction near 0 and near 1 are the same
            worst = std::max({worst, wrap(now.first - reference.first), wrap(now.second - reference.second)});
        }
        CHECK(worst < 0.01f);  // without snapping this moves across the whole texel
    }
    // Not vacuous: unstabilized cascades do shimmer.
    settings.stabilize = false;
    const auto a = texel_position(camera_at({0.0f, 0.0f, 2.0f}, {1.0f, 0.0f, 0.0f}), 0, 1024.0f);
    const auto b = texel_position(camera_at({0.137f, -0.211f, 2.0f}, {1.0f, 0.0f, 0.0f}), 0, 1024.0f);
    CHECK(std::fabs(a.first - b.first) + std::fabs(a.second - b.second) > 0.05f);
}

TEST_CASE("Cascades: size does not depend on where the camera looks, texel size follows the radius, and cascade selection uses depth")
{
    const CascadeSettings settings;
    const Float3 sun = normalize(Float3{-0.40f, -0.55f, 0.73f});
    const ShadowCascades a = fit_shadow_cascades(camera_at({0.0f, 0.0f, 2.0f}, {1.0f, 0.0f, 0.0f}), sun, settings);
    const ShadowCascades b = fit_shadow_cascades(camera_at({50.0f, 9.0f, 30.0f}, {-0.3f, 0.5f, -0.8f}), sun, settings);
    REQUIRE(a.count == 4);
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(a.cascades[i].radius == b.cascades[i].radius);
        CHECK(a.cascades[i].texel_world_size == b.cascades[i].texel_world_size);
        CHECK(std::fabs(a.cascades[i].texel_world_size - 2.0f * a.cascades[i].radius / 2048.0f) < 1e-6f);
        CHECK(a.cascades[i].radius > 0.0f);
        if (i > 0) {
            CHECK(a.cascades[i].radius > a.cascades[i - 1].radius);  // farther slices cover more
            CHECK(a.cascades[i].split_near == a.cascades[i - 1].split_far);
        }
    }
    CHECK(select_cascade(a, 0.5f) == 0);
    CHECK(select_cascade(a, a.cascades[0].split_far - 0.001f) == 0);
    CHECK(select_cascade(a, a.cascades[0].split_far + 0.001f) == 1);
    CHECK(select_cascade(a, a.cascades[3].split_far - 0.001f) == 3);
    CHECK(select_cascade(a, 1000.0f) == 4);  // beyond the shadow distance: no shadow
}

TEST_CASE("Cascades: a light straight overhead or straight below works, and invalid input yields no cascades")
{
    const CascadeSettings settings;
    const CascadeCamera cam = camera_at({0.0f, 0.0f, 5.0f}, {1.0f, 0.0f, 0.0f});
    for (const Float3 sun : {Float3{0.0f, 0.0f, 1.0f}, Float3{0.0f, 0.0f, -1.0f}}) {
        const ShadowCascades set = fit_shadow_cascades(cam, sun, settings);
        REQUIRE(set.count == 4);
        for (uint32_t i = 0; i < 4; ++i) {
            for (const float f : set.cascades[i].view_proj.m) CHECK(std::isfinite(f));
        }
    }
    CascadeSettings bad = settings;
    bad.count = 0;
    CHECK(fit_shadow_cascades(cam, {0.0f, 0.0f, 1.0f}, bad).count == 0);
    bad = settings;
    bad.count = 5;
    CHECK(fit_shadow_cascades(cam, {0.0f, 0.0f, 1.0f}, bad).count == 0);
    bad = settings;
    bad.shadow_distance = bad.near_plane;
    CHECK(fit_shadow_cascades(cam, {0.0f, 0.0f, 1.0f}, bad).count == 0);
    bad = settings;
    bad.resolution = 0;
    CHECK(fit_shadow_cascades(cam, {0.0f, 0.0f, 1.0f}, bad).count == 0);
    CHECK(fit_shadow_cascades(cam, {0.0f, 0.0f, 0.0f}, settings).count == 0);  // no sun direction
    CascadeCamera nan_cam = cam;
    nan_cam.position.x = std::nanf("");
    CHECK(fit_shadow_cascades(nan_cam, {0.0f, 0.0f, 1.0f}, settings).count == 0);
    CascadeCamera no_dir = cam;
    no_dir.forward = {};
    CHECK(fit_shadow_cascades(no_dir, {0.0f, 0.0f, 1.0f}, settings).count == 0);
}
