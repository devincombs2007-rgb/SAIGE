#pragma once

// An immutable broadphase over static collision geometry (axis-aligned boxes and triangles), with the queries a game needs from it:
// ray casts, region queries, capsule sweeps and overlaps and oriented-box sweeps and overlaps for vehicles (issue #43, physics foundations).
//
// It is a bounding-volume hierarchy built once, deterministically: the same inputs give the same tree in the same build, and every query
// result is a pure function of the geometry and the query. Results are identical to the brute-force sweep over every primitive: the
// hierarchy only decides which primitives need to be looked at, and primitives are then examined in ascending index order with the same
// exact-time ordering and tie rule as capsule_sweep.hpp and capsule_triangle.hpp (boxes first, then triangles; an exact tie goes to the
// lower index, and a box wins an exact tie against a triangle). It adds no second controller or movement system: CapsuleCharacter keeps
// its own geometry, and this is what a host can use to cull what it hands to it, to answer line-of-sight and ground-probe queries, or to
// pick candidates for a vehicle.
//
// Ids. Primitives are numbered boxes first (0 .. boxes - 1), then triangles. `StaticPrimitive` names one by kind and index into the
// span it was built from.
//
// Precision. Ray queries are analytic in double and accept any finite coordinates. Capsule queries share the supported envelope of
// capsule_sweep.hpp (chunk-local, +-max_local_extent, at most max_sweep_length) and throw std::invalid_argument outside it, whatever the
// geometry holds. Geometry may extend anywhere.
//
// Cost. Build is O(n log n); a ray or a capsule sweep visits O(log n + k) nodes for k primitives near it. Queries allocate small scratch
// vectors; hold a `QueryScratch` across calls to avoid that. A StaticCollision is safe to query from several threads (it is const).

#include "saige/capsule_sweep.hpp"
#include "saige/capsule_triangle.hpp"
#include "saige/oriented_box.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace saige {

struct StaticPrimitive {
    enum class Kind : std::uint8_t { Box, Triangle };
    Kind kind{Kind::Box};
    std::size_t index{};  // into the boxes or the triangles the collision was built from
    bool operator==(const StaticPrimitive&) const = default;
};

struct RayHit {
    double distance{};      // along the (normalized) ray, in meters; 0 when the origin is inside a box
    Vec3 point;             // origin + direction * distance, rounded to float
    Vec3 normal;            // unit; faces the ray's origin (for a box entered from outside, the entry face's outward normal)
    StaticPrimitive primitive;
    bool started_inside{false};  // a box hit at distance 0 with the origin strictly inside it (not merely on its boundary)
};

// How a ray treats a surface it only touches.
//  * Solid (the default, unchanged): every touching counts, including an origin on a box's boundary moving away from it (a hit at distance 0, normal = the reverse of the
//    direction) and a triangle hit at distance 0.
//  * Surface: a ray is a probe of what lies ahead, so a box it merely touches is not hit and the search goes on to what lies beyond it: an origin on the box's boundary that
//    moves outward or along a face, and a ray running in a face's plane. An origin on the boundary moving in is a hit at distance 0 with that face's outward normal (on an
//    edge, the face the direction is most aligned with; the lowest axis on a tie). Triangles are the same in both modes: they have no interior, so an origin on one is in
//    contact with it (distance 0) and a ray in its plane misses it.
enum class RayMode : std::uint8_t { Solid, Surface };

struct StaticCapsuleHit {
    CapsuleHit hit;              // as the primitive sweeps report it: t, stop, contact_t, normal, point, started_overlapping
    StaticPrimitive primitive;   // which primitive; hit.index is its index within its own kind
};

struct StaticOrientedBoxHit {
    OrientedBoxHit hit;          // see oriented_box.hpp; hit.index is the primitive's index within its own kind
    StaticPrimitive primitive;
};

struct StaticOrientedBoxOverlap {
    OrientedBoxOverlap overlap;  // normal from the primitive toward the box, depth beyond the contact tolerance
    StaticPrimitive primitive;
};

// Reusable buffers for queries, so a frame loop allocates nothing after the first call.
struct QueryScratch {
    std::vector<std::uint32_t> ids;
    std::vector<Aabb> boxes;
    std::vector<Triangle> triangles;
    std::vector<std::size_t> box_ids;
    std::vector<std::size_t> triangle_ids;
};

class StaticCollision {
public:
    StaticCollision() = default;

    // Throws std::invalid_argument for a non-finite or degenerate box or triangle (the same validity the sweeps require). The geometry is
    // copied; the spans need not outlive the call.
    StaticCollision(std::span<const Aabb> boxes, std::span<const Triangle> triangles);

    [[nodiscard]] std::size_t box_count() const noexcept { return boxes_.size(); }
    [[nodiscard]] std::size_t triangle_count() const noexcept { return triangles_.size(); }
    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::uint32_t depth() const noexcept { return depth_; }
    [[nodiscard]] std::span<const Aabb> boxes() const noexcept { return boxes_; }
    [[nodiscard]] std::span<const Triangle> triangles() const noexcept { return triangles_; }

    // The nearest primitive along the ray, or nothing within `max_distance`. `direction` need not be unit length, only finite and non-zero.
    // Boxes are solid (an origin inside one hits at distance 0); triangles are two-sided. An exact tie in distance goes to the lower id.
    // Throws std::invalid_argument for a non-finite origin or direction, a zero direction, or a negative or non-finite max_distance.
    [[nodiscard]] std::optional<RayHit> raycast(Vec3 origin, Vec3 direction, float max_distance) const;
    [[nodiscard]] std::optional<RayHit> raycast(Vec3 origin, Vec3 direction, float max_distance, RayMode mode) const;

    // Appends the primitives whose bounds overlap `region` (touching counts), in ascending id order (boxes, then triangles), to `out`.
    void query_region(const Aabb& region, std::vector<StaticPrimitive>& out) const;

    // The first contact of the capsule moving by `displacement`, the same result as sweeping the capsule against every box and triangle
    // (see the ordering rule above), found by examining only the primitives near the swept volume. Throws like sweep_capsule_boxes,
    // including for a sweep outside the supported envelope.
    [[nodiscard]] std::optional<StaticCapsuleHit> sweep_capsule(const Capsule& capsule, Vec3 displacement) const;
    [[nodiscard]] std::optional<StaticCapsuleHit> sweep_capsule(const Capsule& capsule, Vec3 displacement, QueryScratch& scratch) const;

    // The first contact of an oriented box translating by `displacement` with its orientation fixed (oriented_box.hpp): exact swept separating-axis
    // tests against the primitives near the swept volume, equal to sweeping every box and triangle (earliest exact contact time; an exact tie goes to the
    // deeper initial overlap, then the lower id, boxes before triangles). `stop` is clear of every primitive near the path. Throws like
    // check_oriented_box_sweep_envelope, whatever the geometry holds.
    [[nodiscard]] std::optional<StaticOrientedBoxHit> sweep_oriented_box(const OrientedBox& box, Vec3 displacement) const;
    [[nodiscard]] std::optional<StaticOrientedBoxHit> sweep_oriented_box(const OrientedBox& box, Vec3 displacement, QueryScratch& scratch) const;

    // The deepest penetration of an oriented box into any primitive (more than the contact tolerance), the lowest id on a tie.
    [[nodiscard]] std::optional<StaticOrientedBoxOverlap> overlap_oriented_box(const OrientedBox& box) const;
    [[nodiscard]] std::optional<StaticOrientedBoxOverlap> overlap_oriented_box(const OrientedBox& box, QueryScratch& scratch) const;
    // Appends every primitive the box penetrates by more than the tolerance, in ascending id order.
    void overlaps_oriented_box(const OrientedBox& box, std::vector<StaticOrientedBoxOverlap>& out) const;
    void overlaps_oriented_box(const OrientedBox& box, std::vector<StaticOrientedBoxOverlap>& out, QueryScratch& scratch) const;

    // Whether the capsule overlaps any primitive by more than the contact tolerance (capsule_box_push_out / capsule_triangle_push_out).
    [[nodiscard]] bool overlaps_capsule(const Capsule& capsule) const;
    [[nodiscard]] bool overlaps_capsule(const Capsule& capsule, QueryScratch& scratch) const;

private:
    struct Node {
        float lo[3];
        float hi[3];
        std::uint32_t first{};  // interior: index of the left child (the right child follows its subtree's left); leaf: first slot in order_
        std::uint32_t count{};  // 0 for an interior node
        std::uint32_t right{};  // interior: index of the right child
    };

    void build();
    void gather(const float lo[3], const float hi[3], std::vector<std::uint32_t>& ids) const;

    std::vector<Aabb> boxes_;
    std::vector<Triangle> triangles_;
    std::vector<Node> nodes_;
    std::vector<std::uint32_t> order_;  // primitive ids, leaves refer to ranges of it
    std::uint32_t depth_{};
};

} // namespace saige
