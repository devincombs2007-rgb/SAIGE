#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/shadow_cascades.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>

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

namespace {

bool all_finite(const ShadowCascades& c)
{
    for (uint32_t i = 0; i < c.count; ++i) {
        const ShadowCascade& k = c.cascades[i];
        if (!std::isfinite(k.center.x) || !std::isfinite(k.center.y) || !std::isfinite(k.center.z) || !std::isfinite(k.radius) || !(k.radius > 0.0f) ||
            !std::isfinite(k.split_near) || !std::isfinite(k.split_far) || !(k.split_far > k.split_near) || !std::isfinite(k.texel_world_size) ||
            !(k.texel_world_size > 0.0f) || !std::isfinite(k.depth_range) || !(k.depth_range > 0.0f)) {
            return false;
        }
        for (int r = 0; r < 4; ++r) {
            for (int col = 0; col < 4; ++col) {
                if (!std::isfinite(k.view_proj.at(r, col))) return false;
            }
        }
    }
    return true;
}

bool is_empty(const ShadowCascades& c)
{
    if (c.count != 0) return false;
    for (const ShadowCascade& k : c.cascades) {
        if (k.radius != 0.0f || k.split_far != 0.0f || k.texel_world_size != 0.0f) return false;
    }
    return true;
}

} // namespace

TEST_CASE("Cascades: non-finite and degenerate inputs publish no cascade, and extreme finite inputs publish only finite ones")
{
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    const Float3 sun = normalize(Float3{0.3f, 0.2f, 0.9f});
    const CascadeCamera good = camera_at({0.0f, 0.0f, 2.0f}, {1.0f, 0.0f, 0.0f});
    CHECK(all_finite(fit_shadow_cascades(good, sun, {})) && fit_shadow_cascades(good, sun, {}).count == 4);

    // Core review of #77: aspect = +inf and caster_margin = NaN used to report count = 4 (the latter with a NaN depth projection).
    const auto rejected = [&](CascadeCamera camera, CascadeSettings settings) { return is_empty(fit_shadow_cascades(camera, sun, settings)); };
    CascadeCamera cam = good;
    cam.aspect = inf;
    CHECK(rejected(cam, {}));
    cam = good;
    cam.aspect = nan;
    CHECK(rejected(cam, {}));
    cam = good;
    cam.fov_y_radians = nan;
    CHECK(rejected(cam, {}));
    for (const float margin : {nan, inf, -inf, -1.0f}) {
        CascadeSettings s;
        s.caster_margin = margin;
        CHECK(rejected(good, s));
    }
    CascadeSettings zero_margin;
    zero_margin.caster_margin = 0.0f;
    CHECK(all_finite(fit_shadow_cascades(good, sun, zero_margin)) && fit_shadow_cascades(good, sun, zero_margin).count == 4);
    for (const float lambda : {nan, inf, -inf}) {
        CascadeSettings s;
        s.split_lambda = lambda;
        CHECK(rejected(good, s));
    }
    cam = good;
    cam.position = {nan, 0.0f, 0.0f};
    CHECK(rejected(cam, {}));
    cam = good;
    cam.forward = {0.0f, 0.0f, 0.0f};
    CHECK(rejected(cam, {}));
    CHECK(is_empty(fit_shadow_cascades(good, Float3{nan, 0.0f, 1.0f}, {})));
    CHECK(is_empty(fit_shadow_cascades(good, Float3{0.0f, 0.0f, 0.0f}, {})));
    for (const auto& bad_range : {std::pair<float, float>{nan, 100.0f}, {0.1f, nan}, {0.1f, inf}, {0.0f, 100.0f}, {-1.0f, 100.0f}, {50.0f, 50.0f}, {60.0f, 50.0f}}) {
        CascadeSettings s;
        s.near_plane = bad_range.first;
        s.shadow_distance = bad_range.second;
        CHECK(rejected(good, s));
    }
    for (const uint32_t count : {0u, 5u, 1000u}) {
        CascadeSettings s;
        s.count = count;
        CHECK(rejected(good, s));
    }
    CascadeSettings no_resolution;
    no_resolution.resolution = 0;
    CHECK(rejected(good, no_resolution));

    // Finite but extreme ranges: f / n overflows a float, and derived radii or matrices can overflow. Whatever comes back is either empty
    // or entirely finite with strictly increasing splits; a partial cascade count is never published.
    const float max = std::numeric_limits<float>::max(), tiny = std::numeric_limits<float>::min();
    int accepted = 0, refused = 0;
    for (const float near_plane : {tiny, 1e-30f, 1e-10f, 0.1f, 1e10f}) {
        for (const float far_plane : {1.0f, 1e10f, 1e30f, max}) {
            for (const float aspect : {1e-6f, 1.0f, 1e6f, max}) {
                for (const uint32_t count : {1u, 4u}) {
                    CascadeSettings s;
                    s.near_plane = near_plane;
                    s.shadow_distance = far_plane;
                    s.count = count;
                    CascadeCamera camera = good;
                    camera.aspect = aspect;
                    const auto splits = cascade_splits(near_plane, far_plane, count, s.split_lambda);
                    if (splits[count] != 0.0f) {
                        for (uint32_t i = 1; i <= count; ++i) CHECK(std::isfinite(splits[i]) && splits[i] > splits[i - 1]);
                    }
                    const ShadowCascades fitted = fit_shadow_cascades(camera, sun, s);
                    if (fitted.count == 0) {
                        CHECK(is_empty(fitted));
                        ++refused;
                    } else {
                        CHECK(fitted.count == count && all_finite(fitted));
                        ++accepted;
                    }
                }
            }
        }
    }
    CHECK(accepted > 0 && refused > 0);  // the sweep exercises both outcomes
    // An extreme but representable ratio is split robustly rather than overflowing f / n.
    const auto wide = cascade_splits(1e-30f, 1e30f, 4, 1.0f);
    CHECK(wide[4] == 1e30f && wide[0] == 1e-30f && wide[1] > wide[0] && wide[2] > wide[1] && wide[3] > wide[2] && wide[4] > wide[3]);
}
