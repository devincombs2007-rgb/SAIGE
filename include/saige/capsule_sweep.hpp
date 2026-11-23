#pragma once

// Swept vertical capsule against axis-aligned boxes: the collision primitive for the character course (issue #62, step 1).
//
// Z is up (docs/standards/coordinate-system.md). A capsule is a segment parallel to +Z swept by a sphere of `radius`; the
// character never tilts, so only that orientation is supported. Everything is a pure function of its arguments: no state, no
// allocation, no clock, so replays are identical for the same build. Internal math is double precision on the float inputs.
//
// The sweep is exact, not stepped. The capsule's first contact with a box is the first time a sphere center, moving along the
// displacement, touches the box grown by the capsule: a box extended downward by the segment length, then rounded by the radius
// (faces, edge cylinders and corner spheres). Contact normals therefore come out right on faces, edges and corners.
//
// Adoption note: Aabb has the layout of StaticBox in character_motor.hpp (#65), so a motor can pass its boxes by conversion.
//
// Supported envelope (issue #43 decision). Positions are chunk-local meters. A sweep is supported when the capsule's bounding box
// at the start and at the end of the displacement lies within +-max_local_extent on every axis and the displacement is at most
// max_sweep_length long. At that scale a float coordinate resolves 2^-16 m (about 1.5e-5), so the result can be applied exactly in
// float. A query outside the envelope throws std::invalid_argument: it never returns a result computed at lower precision. The
// caller (Core) rebases larger world coordinates before converting to float and subdivides longer motions, taking the first hit in
// motion order. Obstacles may extend beyond the envelope (a large ground box is fine); only the capsule's path is bounded.
//
// Applying a hit. `CapsuleHit::stop` is the feet position to adopt, already rounded to float: it is the latest float position on
// the way to the contact at which the capsule is still clear of every box in the query. Do not recompute `feet + displacement * t`
// in float: at the supported scale that rounds to a position up to one coordinate step past the contact. `t` is the fraction of the
// displacement that `stop` corresponds to and is kept for ordering and diagnostics.

#include "saige/world.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace saige {

struct Aabb {
    Vec3 min;
    Vec3 max;
};

// A vertical capsule. `feet` is its lowest point; `height` is the full height including both hemispherical caps and must be at
// least 2 * radius (equal means a sphere).
struct Capsule {
    Vec3 feet;
    float radius{};
    float height{};
};

// The supported envelope (see above).
inline constexpr float max_local_extent = 256.0f;
inline constexpr float max_sweep_length = 64.0f;

struct CapsuleHit {
    float t{};                       // fraction of the displacement at `stop`, in [0, 1]
    Vec3 stop;                       // feet position to adopt: float, clear of the obstacles, within a few coordinate steps of the contact
    Vec3 normal;                     // unit length, pointing from the box toward the capsule
    Vec3 point;                      // closest point on the box to the capsule's axis at contact
    bool converged{true};            // false only for a triangle sweep that hit its iteration cap: `stop` is clear but may be short of the contact
    bool started_overlapping{};      // the capsule already penetrated the box at t = 0; normal is the push-out direction
    std::size_t index{};             // sweep_capsule_boxes: which box
};

// Throws std::invalid_argument unless the capsule's sweep by `displacement` is inside the supported envelope (see above). Called by
// every sweep and push-out query; exposed so that a caller can reject or subdivide a motion before building a query.
void check_sweep_envelope(const Capsule& capsule, Vec3 displacement);

// Gap between the capsule's surface and the box: zero when touching, negative when overlapping. (Measured from the axis, so the
// value is exact whenever the axis is outside the box and saturates at -radius once the axis is inside.)
[[nodiscard]] float capsule_box_gap(const Capsule& capsule, const Aabb& box);

// First contact while moving by `displacement`, or no value if the path is clear for the whole displacement. A capsule that
// merely touches a box (within 1e-5) and is not moving into it does not hit it, so it can slide along a surface. Throws
// std::invalid_argument for a non-finite or degenerate capsule, box or displacement.
[[nodiscard]] std::optional<CapsuleHit> sweep_capsule_box(const Capsule& capsule, Vec3 displacement, const Aabb& box);

// The earliest hit over all boxes; ties go to the lowest index. The result's `index` identifies the box.
[[nodiscard]] std::optional<CapsuleHit> sweep_capsule_boxes(const Capsule& capsule, Vec3 displacement, std::span<const Aabb> boxes);

// The smallest translation that separates an overlapping capsule from a box so that they just touch, or no value when they do not
// overlap (within 1e-5). The direction is the push-out normal `sweep_capsule_box` reports for a capsule that starts overlapping.
// Contact recovery for a controller: apply the push, plus a small skin, then continue. Throws like sweep_capsule_box.
[[nodiscard]] std::optional<Vec3> capsule_box_push_out(const Capsule& capsule, const Aabb& box);

// Whether a contact normal is ground the character may stand on: it faces up and its angle from +Z is at most `max_slope_radians`.
// The limit must be in [0, pi/2): 0 allows level ground only, and a negative, non-finite or >= pi/2 limit makes nothing walkable
// (a limit that reaches a vertical wall is not a slope limit). A ceiling or a wall is never ground. A non-unit or non-finite
// normal is not walkable.
[[nodiscard]] bool is_walkable(Vec3 normal, float max_slope_radians) noexcept;

namespace detail {

// The shared contact-to-stop reconstruction for boxes and triangles. `exact_t` is the contact fraction computed in double. Returns
// the float feet position to adopt: the float nearest `feet + displacement * exact_t` if `clear` accepts it, otherwise the nearest
// float to an earlier point on the path, backing off geometrically from one coordinate step (so at most about twice the necessary
// distance) and ending at `feet` itself, which the caller guarantees is acceptable. `clear(Vec3)` judges a float pose against every
// obstacle in the query. Deterministic: no state, a bounded number of iterations.
template <class Clear>
[[nodiscard]] Vec3 conservative_stop(Vec3 feet, Vec3 displacement, double exact_t, Clear&& clear)
{
    const double length = std::sqrt(static_cast<double>(displacement.x) * displacement.x + static_cast<double>(displacement.y) * displacement.y +
                                    static_cast<double>(displacement.z) * displacement.z);
    const auto at = [&](double t) {
        return Vec3{static_cast<float>(static_cast<double>(feet.x) + static_cast<double>(displacement.x) * t),
                    static_cast<float>(static_cast<double>(feet.y) + static_cast<double>(displacement.y) * t),
                    static_cast<float>(static_cast<double>(feet.z) + static_cast<double>(displacement.z) * t)};
    };
    Vec3 candidate = at(exact_t);
    if (length == 0.0 || clear(candidate)) return candidate;
    // One coordinate step at the largest magnitude involved, expressed as a fraction of the displacement.
    const double magnitude = std::max({std::abs(static_cast<double>(candidate.x)), std::abs(static_cast<double>(candidate.y)), std::abs(static_cast<double>(candidate.z)), 1.0});
    double step = std::ldexp(magnitude, -23) / length;  // 2^-23 relative: one float coordinate step
    for (int i = 0; i < 48; ++i, step *= 2.0) {
        const double t = exact_t - step;
        if (t <= 0.0) break;
        candidate = at(t);
        if (clear(candidate)) return candidate;
    }
    return feet;
}

} // namespace detail

} // namespace saige
