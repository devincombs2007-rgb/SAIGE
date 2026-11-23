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

#include "saige/world.hpp"

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

struct CapsuleHit {
    float t{};                       // fraction of the displacement travelled at first contact, in [0, 1]
    Vec3 normal;                     // unit length, pointing from the box toward the capsule
    Vec3 point;                      // closest point on the box to the capsule's axis at contact
    bool started_overlapping{};      // the capsule already penetrated the box at t = 0; normal is the push-out direction
    std::size_t index{};             // sweep_capsule_boxes: which box
};

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

// Whether a contact normal is ground the character may stand on: its angle from +Z is at most `max_slope_radians`.
// A non-unit or non-finite normal is not walkable.
[[nodiscard]] bool is_walkable(Vec3 normal, float max_slope_radians) noexcept;

} // namespace saige
