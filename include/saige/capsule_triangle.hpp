#pragma once

// Swept vertical capsule against triangles: the collision primitive for ramps and sloped surfaces (issue #62, step 2).
//
// It extends capsule_sweep.hpp (#69) and reuses its Capsule and CapsuleHit. Z is up. Triangles are two-sided: the contact normal
// always points from the triangle toward the capsule, whichever way the triangle faces. Everything is a pure function of its
// arguments: no state, no allocation, no clock, so replays are identical for the same build. Internal math is double precision.
//
// Method. The capsule is a vertical core segment inflated by `radius`, so "the capsule touches the triangle" means "the exact
// distance between the core segment and the triangle equals the radius". That distance is computed from its features: the segment
// crossing the face, an endpoint against the face, edges or vertices, and the segment against each edge. The sweep is conservative
// advancement along the separating axis between the closest points. For a pure translation that step is rigorous (the capsule cannot
// reach the triangle sooner), so a sweep can neither tunnel nor stop early beyond the tolerance, however long the displacement,
// and a path that is moving away or parallel is rejected immediately. Contact normals come out right on faces, edges and vertices.
//
// Touching rule (the same as the box sweep): a capsule within 1e-5 of a triangle that is not moving into it does not hit it, so it
// can slide along a surface. A capsule that starts penetrating reports started_overlapping with the push-out direction as the normal.
// Deep penetration, where the core segment crosses the triangle, is resolved along the face normal to the nearer side; a sideways
// exit through the triangle's boundary is not considered. Contact recovery is meant for small corrections, not for teleports into
// geometry.
//
// Cost is linear in the number of triangles; each is rejected by its bounds against the swept capsule first. There is no
// acceleration structure yet (measure before adding one; see tests/capsule_triangle_test.cpp for a timing of the ramp-sized case).

#include "saige/capsule_sweep.hpp"

#include <optional>
#include <span>

namespace saige {

struct Triangle {
    Vec3 a, b, c;
};

// Non-finite coordinates or an (almost) zero area. The sweep functions throw std::invalid_argument for an invalid triangle.
[[nodiscard]] bool is_valid_triangle(const Triangle& triangle) noexcept;

// Unit normal of (b - a) x (c - a). Throws std::invalid_argument for an invalid triangle.
[[nodiscard]] Vec3 triangle_normal(const Triangle& triangle);

// Gap between the capsule's surface and the triangle: zero when touching, negative when overlapping. Exact whenever the core segment
// does not cross the triangle, and -radius when it does.
[[nodiscard]] float capsule_triangle_gap(const Capsule& capsule, const Triangle& triangle);

// First contact while moving by `displacement`, or no value if the path is clear for the whole displacement. Throws
// std::invalid_argument for a non-finite or degenerate capsule, triangle or displacement.
[[nodiscard]] std::optional<CapsuleHit> sweep_capsule_triangle(const Capsule& capsule, Vec3 displacement, const Triangle& triangle);

// The earliest hit over all triangles; ties go to the lowest index. The result's `index` identifies the triangle.
[[nodiscard]] std::optional<CapsuleHit> sweep_capsule_triangles(const Capsule& capsule, Vec3 displacement, std::span<const Triangle> triangles);

// The smallest translation that separates an overlapping capsule from a triangle so that they just touch, or no value when they do
// not overlap (within 1e-5). For a capsule whose core segment crosses the triangle see the note above.
[[nodiscard]] std::optional<Vec3> capsule_triangle_push_out(const Capsule& capsule, const Triangle& triangle);

} // namespace saige
