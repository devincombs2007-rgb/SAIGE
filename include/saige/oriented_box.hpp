#pragma once

// Oriented boxes against the static collision primitives (axis-aligned boxes and triangles): the query a vehicle needs (issue #43, physics
// foundations; decided with Core on #43).
//
// An OrientedBox is a center, three half extents and an orthonormal frame, fixed for the query. Two queries, both exact in double on the float
// inputs and pure functions of their arguments (no state, no allocation in the pair functions, replays identical for the same build):
//
//  * overlap: whether the box penetrates a primitive by more than `box_contact_tolerance`, with the minimum-translation direction and depth. It is
//    a separating-axis test over the face normals and edge cross products (15 axes against a box, 13 against a triangle), which is exact for
//    convex polyhedra.
//  * sweep: the first contact while the box translates by `displacement` with its orientation fixed. This is a swept separating-axis test over
//    the same axes: for each axis the interval of times the projections overlap, the entry time is the latest entry and the exit the earliest
//    exit, and the box hits when entry <= exit and entry is inside [0, 1]. It is exact (not stepped, not conservative-advance) for translation;
//    a rotating box is out of scope (the caller subdivides the motion and re-orients between steps; rotational sweeps and wheel contacts
//    follow).
//
// What a hit reports.
//  * `contact_t`: the exact contact fraction in double. Hits from different queries or primitives are ordered by it; an exact tie goes to the
//    lower primitive id (StaticCollision), boxes before triangles, the rule of capsule_sweep.hpp.
//  * `normal`: unit, from the primitive toward the box (the direction to push the box to separate it).
//  * `stop`: the center to adopt, rounded to float: the latest float position on the way to the contact at which the box is clear (not
//    overlapping) of every primitive in the query. Do not recompute `center + displacement * t` in float. `t` is the fraction of `stop`.
//  * Initial overlap. If the box already penetrates the primitive by more than the tolerance at t = 0, the hit has `started_overlapping`,
//    t = 0, contact_t = 0, `stop` = the start, `normal` = the minimum-translation direction and `penetration` = its depth; the displacement does
//    not matter. If the box merely touches (the gap is within the tolerance), it is a hit at t = 0 only when the displacement moves into the
//    primitive (negative component along the contact normal beyond rounding); moving along or away from the surface is no hit, so a box
//    resting on a floor slides over it and drives away from a wall it touches.
//
// Envelope (the same decision as capsule_sweep.hpp). Positions are chunk-local: the box's bounding box at the start and at the end of the
// displacement must lie within +-max_local_extent on every axis and the displacement is at most max_sweep_length long; half extents are at
// most `max_box_half_extent`. Outside it the queries throw std::invalid_argument rather than answer at lower precision. Primitives may extend
// anywhere. Overlap tests have no displacement and need only the box inside +-max_local_extent.

#include "saige/capsule_sweep.hpp"
#include "saige/capsule_triangle.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace saige {

struct OrientedBox {
    Vec3 center;
    Vec3 half_extents{0.5f, 0.5f, 0.5f};
    // The box's frame: unit length, mutually perpendicular (within 1e-4), any handedness. Defaults to the world axes.
    Vec3 axis_x{1.0f, 0.0f, 0.0f};
    Vec3 axis_y{0.0f, 1.0f, 0.0f};
    Vec3 axis_z{0.0f, 0.0f, 1.0f};
};

inline constexpr float box_contact_tolerance = 1e-4f;  // meters: a gap or penetration within this is "touching"
inline constexpr float max_box_half_extent = 64.0f;

// A box turned by `yaw_radians` about +Z (the vehicle's heading: local +X forward, +Z up, counter-clockwise from above). Throws for non-finite input
// or a non-positive half extent.
[[nodiscard]] OrientedBox make_oriented_box(Vec3 center, Vec3 half_extents, float yaw_radians);
// A box with the orientation of the quaternion (x, y, z, w), normalized here. Throws for a non-finite or zero quaternion.
[[nodiscard]] OrientedBox make_oriented_box(Vec3 center, Vec3 half_extents, float qx, float qy, float qz, float qw);

// Throws std::invalid_argument for a non-finite value, a half extent that is not in (0, max_box_half_extent], or a frame that is not orthonormal.
void validate_oriented_box(const OrientedBox& box);
// validate_oriented_box plus the sweep envelope above (displacement may be zero).
void check_oriented_box_sweep_envelope(const OrientedBox& box, Vec3 displacement);

struct OrientedBoxOverlap {
    Vec3 normal;   // unit, minimum-translation direction, from the primitive toward the box
    float depth{}; // penetration along it, more than box_contact_tolerance
};

struct OrientedBoxHit {
    float t{};                      // fraction of the displacement at `stop`
    Vec3 stop;                      // box center to adopt (see above)
    double contact_t{};             // exact contact fraction in double
    Vec3 normal;                    // unit, primitive toward box
    float penetration{};            // > box_contact_tolerance exactly when started_overlapping
    bool started_overlapping{};
    std::size_t index{};            // which primitive within its own span (boxes or triangles) a multi-primitive function was given
    bool on_triangle{};             // the primitive is a triangle (else a box)
};

[[nodiscard]] std::optional<OrientedBoxOverlap> oriented_box_aabb_overlap(const OrientedBox& box, const Aabb& aabb);
[[nodiscard]] std::optional<OrientedBoxOverlap> oriented_box_triangle_overlap(const OrientedBox& box, const Triangle& triangle);

// The signed separation along the best separating axis, in double: positive when the box and the primitive are apart (a lower bound on their distance;
// exact for face and edge contacts), zero when they touch, negative when they overlap (minus the penetration depth). A stop is clear when this is >= 0
// against every primitive near the path.
[[nodiscard]] double oriented_box_aabb_gap(const OrientedBox& box, const Aabb& aabb);
[[nodiscard]] double oriented_box_triangle_gap(const OrientedBox& box, const Triangle& triangle);

// The first contact of the translating box with one primitive, or nothing. `stop` is clear of that primitive.
[[nodiscard]] std::optional<OrientedBoxHit> sweep_oriented_box_aabb(const OrientedBox& box, Vec3 displacement, const Aabb& aabb);
[[nodiscard]] std::optional<OrientedBoxHit> sweep_oriented_box_triangle(const OrientedBox& box, Vec3 displacement, const Triangle& triangle);

// The first contact against several primitives (boxes and triangles): the earliest by contact_t, an exact tie to the lower index, a box winning
// an exact tie against a triangle; `index` is within its own span. `stop` is clear of every primitive given.
[[nodiscard]] std::optional<OrientedBoxHit> sweep_oriented_box(const OrientedBox& box, Vec3 displacement, std::span<const Aabb> boxes,
                                                               std::span<const Triangle> triangles);

// The latest float center on the way to `contact_t` (0..1) at which the box overlaps none of the primitives by more than zero. Exposed for
// callers that combine sweeps from several sources.
[[nodiscard]] Vec3 oriented_box_stop(const OrientedBox& box, Vec3 displacement, double contact_t, std::span<const Aabb> boxes,
                                     std::span<const Triangle> triangles);

} // namespace saige
