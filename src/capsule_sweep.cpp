#include "saige/capsule_sweep.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace saige {
namespace {

using D3 = std::array<double, 3>;

constexpr double touch_tolerance = 1e-5;  // the same contact tolerance the character motor uses

bool finite(Vec3 v) noexcept {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

D3 to_d3(Vec3 v) noexcept {
    return {v.x, v.y, v.z};
}

Vec3 to_vec3(const D3& v) noexcept {
    return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])};
}

double dot(const D3& a, const D3& b) noexcept {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

D3 sub(const D3& a, const D3& b) noexcept {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

D3 clamp3(const D3& p, const D3& lo, const D3& hi) noexcept {
    return {std::clamp(p[0], lo[0], hi[0]), std::clamp(p[1], lo[1], hi[1]), std::clamp(p[2], lo[2], hi[2])};
}

void validate(const Capsule& capsule) {
    if (!finite(capsule.feet) || !std::isfinite(capsule.radius) || !std::isfinite(capsule.height) || capsule.radius <= 0.0f ||
        capsule.height < 2.0f * capsule.radius) {
        throw std::invalid_argument("Capsule must be finite with a positive radius and a height of at least twice the radius");
    }
}

void validate(const Capsule& capsule, const Aabb& box) {
    validate(capsule);
    if (!finite(box.min) || !finite(box.max) || box.min.x >= box.max.x || box.min.y >= box.max.y || box.min.z >= box.max.z) {
        throw std::invalid_argument("Box must be finite with min below max on every axis");
    }
}

// Signed gap between the capsule surface and the box for a capsule whose feet are at (x, y, z), all in double. A vertical segment and a
// box are separable per axis: x and y distances, and the gap between the z intervals. Saturates at -radius once the axis is inside.
double box_gap(double x, double y, double z, double radius, double height, const Aabb& box) noexcept {
    const double bottom = z + radius;
    const double top = z + height - radius;
    const double dx = std::max({static_cast<double>(box.min.x) - x, 0.0, x - static_cast<double>(box.max.x)});
    const double dy = std::max({static_cast<double>(box.min.y) - y, 0.0, y - static_cast<double>(box.max.y)});
    const double dz = std::max({static_cast<double>(box.min.z) - top, 0.0, bottom - static_cast<double>(box.max.z)});
    return std::sqrt(dx * dx + dy * dy + dz * dz) - radius;
}

double box_gap(const Capsule& capsule, Vec3 feet, const Aabb& box) noexcept {
    return box_gap(feet.x, feet.y, feet.z, capsule.radius, capsule.height, box);
}

struct RawHit {
    CapsuleHit hit;  // t is the float of exact_t; stop is not set yet
    double exact_t{};
};

// Smallest t >= 0 at which origin + t * direction enters the sphere; 0 if already inside and moving toward the center.
std::optional<double> ray_sphere(const D3& origin, const D3& direction, const D3& center, double radius) noexcept {
    const D3 m = sub(origin, center);
    const double a = dot(direction, direction);
    const double b = dot(m, direction);
    const double c = dot(m, m) - radius * radius;
    if (c <= 0.0) return b < 0.0 ? std::optional<double>(0.0) : std::nullopt;
    if (a == 0.0 || b >= 0.0) return std::nullopt;
    const double discriminant = b * b - a * c;
    if (discriminant < 0.0) return std::nullopt;
    return std::max(0.0, (-b - std::sqrt(discriminant)) / a);
}

// The same for a finite cylinder around the axis-parallel line through (center_i, center_j), spanning [lo, hi] along `axis`.
std::optional<double> ray_cylinder(const D3& origin, const D3& direction, std::size_t axis, double center_i, double center_j,
                                   double lo, double hi, double radius) noexcept {
    const std::size_t i = (axis + 1) % 3;
    const std::size_t j = (axis + 2) % 3;
    const double mi = origin[i] - center_i;
    const double mj = origin[j] - center_j;
    const double a = direction[i] * direction[i] + direction[j] * direction[j];
    const double b = mi * direction[i] + mj * direction[j];
    const double c = mi * mi + mj * mj - radius * radius;
    double t = 0.0;
    if (c > 0.0) {
        if (a == 0.0 || b >= 0.0) return std::nullopt;
        const double discriminant = b * b - a * c;
        if (discriminant < 0.0) return std::nullopt;
        t = std::max(0.0, (-b - std::sqrt(discriminant)) / a);
    } else if (!(b < 0.0)) {
        return std::nullopt;
    }
    const double along = origin[axis] + t * direction[axis];
    if (along < lo || along > hi) return std::nullopt;
    return t;
}

// The union of a cylinder and its two end spheres: an axis-parallel segment swept by a sphere.
std::optional<double> ray_segment_capsule(const D3& origin, const D3& direction, std::size_t axis, const D3& at, double lo,
                                          double hi, double radius) noexcept {
    const std::size_t i = (axis + 1) % 3;
    const std::size_t j = (axis + 2) % 3;
    std::optional<double> best = ray_cylinder(origin, direction, axis, at[i], at[j], lo, hi, radius);
    for (const double end : {lo, hi}) {
        D3 center = at;
        center[axis] = end;
        if (const auto t = ray_sphere(origin, direction, center, radius)) {
            if (!best || *t < *best) best = t;
        }
    }
    return best;
}

// Ray against the axis-aligned slab box [lo, hi], restricted to t in [0, 1]. The entry time is 0 when the origin is inside.
std::optional<double> ray_box(const D3& origin, const D3& direction, const D3& lo, const D3& hi) noexcept {
    double t_enter = 0.0;
    double t_exit = 1.0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (direction[axis] == 0.0) {
            if (origin[axis] < lo[axis] || origin[axis] > hi[axis]) return std::nullopt;
            continue;
        }
        double t1 = (lo[axis] - origin[axis]) / direction[axis];
        double t2 = (hi[axis] - origin[axis]) / direction[axis];
        if (t1 > t2) std::swap(t1, t2);
        t_enter = std::max(t_enter, t1);
        t_exit = std::min(t_exit, t2);
        if (t_enter > t_exit) return std::nullopt;
    }
    return t_enter;
}

D3 normalized_or(const D3& v, const D3& fallback) noexcept {
    const double length = std::sqrt(dot(v, v));
    if (length < 1e-12) return fallback;
    return {v[0] / length, v[1] / length, v[2] / length};
}

// Closest point on the original box to the capsule's axis for a capsule whose lowest sphere center is `center`.
D3 axis_contact_point(const D3& center, double segment_length, const Aabb& box) noexcept {
    D3 p{std::clamp(center[0], static_cast<double>(box.min.x), static_cast<double>(box.max.x)),
         std::clamp(center[1], static_cast<double>(box.min.y), static_cast<double>(box.max.y)), 0.0};
    const double bottom = center[2];
    const double top = center[2] + segment_length;
    const double low = std::max(bottom, static_cast<double>(box.min.z));
    const double high = std::min(top, static_cast<double>(box.max.z));
    if (low <= high) {
        p[2] = 0.5 * (low + high);
    } else {
        p[2] = top < box.min.z ? box.min.z : box.max.z;
    }
    return p;
}

struct Overlap {
    D3 normal;
    double depth;  // distance to move along `normal` for the capsule to just touch the box
};

// The push-out for a lowest sphere center `start` against the box grown downward to [lo, hi], or nothing if the capsule is not
// penetrating deeper than the contact tolerance.
std::optional<Overlap> overlap_push(const D3& start, const D3& lo, const D3& hi, double radius) noexcept {
    const D3 nearest = clamp3(start, lo, hi);
    const D3 offset = sub(start, nearest);
    const double distance2 = dot(offset, offset);
    const double shallow = std::max(0.0, radius - touch_tolerance);
    if (distance2 != 0.0 && distance2 >= shallow * shallow) return std::nullopt;
    if (distance2 > 0.0) {
        return Overlap{normalized_or(offset, {0.0, 0.0, 1.0}), radius - std::sqrt(distance2)};
    }
    // The center is inside the grown box: leave through the nearest face (ties: x, y, z; negative before positive).
    double best = std::numeric_limits<double>::infinity();
    D3 normal{0.0, 0.0, 1.0};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double to_min = start[axis] - lo[axis];
        const double to_max = hi[axis] - start[axis];
        if (to_min < best) { best = to_min; normal = {0.0, 0.0, 0.0}; normal[axis] = -1.0; }
        if (to_max < best) { best = to_max; normal = {0.0, 0.0, 0.0}; normal[axis] = 1.0; }
    }
    return Overlap{normal, best + radius};
}

} // namespace

std::optional<Vec3> capsule_box_push_out(const Capsule& capsule, const Aabb& box) {
    validate(capsule, box);
    check_sweep_envelope(capsule, Vec3{0.0f, 0.0f, 0.0f});
    const double r = capsule.radius;
    const double segment = static_cast<double>(capsule.height) - 2.0 * r;
    const D3 start{capsule.feet.x, capsule.feet.y, static_cast<double>(capsule.feet.z) + r};
    const D3 lo{box.min.x, box.min.y, static_cast<double>(box.min.z) - segment};
    const D3 hi{box.max.x, box.max.y, box.max.z};
    const auto overlap = overlap_push(start, lo, hi, r);
    if (!overlap) return std::nullopt;
    return Vec3{static_cast<float>(overlap->normal[0] * overlap->depth), static_cast<float>(overlap->normal[1] * overlap->depth),
                static_cast<float>(overlap->normal[2] * overlap->depth)};
}

float capsule_box_gap(const Capsule& capsule, const Aabb& box) {
    validate(capsule, box);
    return static_cast<float>(box_gap(capsule, capsule.feet, box));
}

void check_sweep_envelope(const Capsule& capsule, Vec3 displacement) {
    validate(capsule);
    if (!finite(displacement)) throw std::invalid_argument("Capsule displacement must be finite");
    const double length = std::sqrt(dot(to_d3(displacement), to_d3(displacement)));
    if (!(length <= static_cast<double>(max_sweep_length))) {
        throw std::invalid_argument("Capsule displacement exceeds max_sweep_length: subdivide the motion");
    }
    const double r = capsule.radius;
    const D3 feet = to_d3(capsule.feet);
    const D3 d = to_d3(displacement);
    constexpr double extent = static_cast<double>(max_local_extent);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        // The capsule's bounding box: radius around the axis in x and y, the full height in z.
        const double low = axis == 2 ? feet[2] : feet[axis] - r;
        const double high = axis == 2 ? feet[2] + capsule.height : feet[axis] + r;
        if (std::min(low, low + d[axis]) < -extent || std::max(high, high + d[axis]) > extent) {
            throw std::invalid_argument("Capsule sweep leaves the supported +-max_local_extent range: rebase to chunk-local coordinates");
        }
    }
}

namespace {

// The exact first contact (in double) with one box. The caller has validated the capsule and the envelope.
std::optional<RawHit> raw_sweep_box(const Capsule& capsule, Vec3 displacement, const Aabb& box) {
    validate(capsule, box);

    const double r = capsule.radius;
    const double segment = static_cast<double>(capsule.height) - 2.0 * r;
    const D3 start{capsule.feet.x, capsule.feet.y, static_cast<double>(capsule.feet.z) + r};  // lowest sphere center
    const D3 d = to_d3(displacement);

    // Moving the whole capsule equals moving its lowest sphere center against the box grown downward by the segment length.
    const D3 lo{box.min.x, box.min.y, static_cast<double>(box.min.z) - segment};
    const D3 hi{box.max.x, box.max.y, box.max.z};


    const auto make_hit = [&](double t, const D3& normal, bool overlapping) {
        RawHit raw;
        raw.exact_t = t;
        raw.hit.t = static_cast<float>(t);
        raw.hit.normal = to_vec3(normal);
        raw.hit.point = to_vec3(axis_contact_point({start[0] + t * d[0], start[1] + t * d[1], start[2] + t * d[2]}, segment, box));
        raw.hit.started_overlapping = overlapping;
        return raw;
    };

    // Penetrating at the start: report the push-out direction and stop the move there.
    if (const auto overlap = overlap_push(start, lo, hi, r)) {
        return make_hit(0.0, overlap->normal, true);
    }

    // Slab test against the box grown by the radius on every side.
    const D3 grown_lo{lo[0] - r, lo[1] - r, lo[2] - r};
    const D3 grown_hi{hi[0] + r, hi[1] + r, hi[2] + r};
    const auto entry = ray_box(start, d, grown_lo, grown_hi);
    if (!entry) return std::nullopt;

    // Where the ray entered the grown box decides which rounded feature can be hit first.
    const D3 p{start[0] + *entry * d[0], start[1] + *entry * d[1], start[2] + *entry * d[2]};
    std::array<bool, 3> low_side{};
    std::array<bool, 3> high_side{};
    int outside_axes = 0;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        low_side[axis] = p[axis] < lo[axis];
        high_side[axis] = p[axis] > hi[axis];
        if (low_side[axis] || high_side[axis]) ++outside_axes;
    }

    double t = *entry;
    if (outside_axes >= 2) {
        // Edge or corner region: only the rounded edges (cylinders with end spheres) can be struck. At a corner the three
        // edges meeting there are tested; in an edge region, the one edge along the remaining axis.
        std::optional<double> best;
        const auto test_edge = [&](std::size_t axis) {
            D3 at{};
            for (std::size_t k = 0; k < 3; ++k) {
                if (k == axis) continue;
                at[k] = low_side[k] ? lo[k] : high_side[k] ? hi[k] : std::clamp(p[k], lo[k], hi[k]);
            }
            if (const auto candidate = ray_segment_capsule(start, d, axis, at, lo[axis], hi[axis], r)) {
                if (!best || *candidate < *best) best = candidate;
            }
        };
        if (outside_axes == 3) {
            for (std::size_t axis = 0; axis < 3; ++axis) test_edge(axis);
        } else {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (!low_side[axis] && !high_side[axis]) test_edge(axis);
            }
        }
        if (!best || *best > 1.0) return std::nullopt;
        t = *best;
    }

    const D3 center{start[0] + t * d[0], start[1] + t * d[1], start[2] + t * d[2]};
    const D3 closest = clamp3(center, lo, hi);
    const D3 normal = normalized_or(sub(center, closest), {0.0, 0.0, 1.0});

    // Touching and not moving into the surface (sliding, or leaving it) is not a hit.
    if (t <= touch_tolerance && dot(normal, d) >= 0.0) return std::nullopt;
    return make_hit(t, normal, false);
}

// Fixes `raw.hit.stop` and `raw.hit.t`: the float feet position to adopt, clear of every box in `boxes` (see detail::conservative_stop).
// A box that is already within the contact tolerance at the start may not be entered any deeper; the box that was hit is held to
// that exactly, the others to half the contact tolerance so a rounding step along a touched wall does not block the motion.
CapsuleHit finalize(const Capsule& capsule, Vec3 displacement, RawHit raw, std::span<const Aabb> boxes, std::size_t hit_index) {
    CapsuleHit hit = raw.hit;
    if (hit.started_overlapping || raw.exact_t <= 0.0) {
        hit.t = 0.0f;
        hit.stop = capsule.feet;
        return hit;
    }
    const auto clear = [&](Vec3 pose) {
        for (std::size_t i = 0; i < boxes.size(); ++i) {
            const double start_gap = box_gap(capsule, capsule.feet, boxes[i]);
            const double allowed = std::min(start_gap, 0.0) - (i == hit_index ? 0.0 : 0.5 * touch_tolerance);
            if (box_gap(capsule, pose, boxes[i]) < allowed) return false;
        }
        return true;
    };
    hit.stop = detail::conservative_stop(capsule.feet, displacement, raw.exact_t, clear);
    const D3 moved = sub(to_d3(hit.stop), to_d3(capsule.feet));
    const D3 d = to_d3(displacement);
    const double along = dot(d, d) > 0.0 ? dot(moved, d) / dot(d, d) : 0.0;
    hit.t = static_cast<float>(std::clamp(along, 0.0, raw.exact_t));
    return hit;
}

} // namespace

std::optional<CapsuleHit> sweep_capsule_box(const Capsule& capsule, Vec3 displacement, const Aabb& box) {
    validate(capsule, box);
    check_sweep_envelope(capsule, displacement);
    const auto raw = raw_sweep_box(capsule, displacement, box);
    if (!raw) return std::nullopt;
    return finalize(capsule, displacement, *raw, std::span<const Aabb>(&box, 1), 0);
}

std::optional<CapsuleHit> sweep_capsule_boxes(const Capsule& capsule, Vec3 displacement, std::span<const Aabb> boxes) {
    // The query (capsule, displacement, envelope) is validated once, up front, so an invalid or out-of-range query throws whatever
    // the set holds (an empty set included); each box is validated by the single-box sweep.
    validate(capsule);
    check_sweep_envelope(capsule, displacement);
    std::optional<RawHit> best;
    std::size_t best_index = 0;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        auto raw = raw_sweep_box(capsule, displacement, boxes[i]);
        // Ordered by the exact contact time in double, before it is rounded to the float `t` of the result: two boxes whose contact
        // times differ by less than a float step are not tied. A tie is exact equality of the double times, and goes to the lowest index.
        if (raw && (!best || raw->exact_t < best->exact_t)) {
            raw->hit.index = i;
            best = raw;
            best_index = i;
        }
    }
    if (!best) return std::nullopt;
    CapsuleHit hit = finalize(capsule, displacement, *best, boxes, best_index);
    hit.index = best_index;
    return hit;
}

bool is_walkable(Vec3 normal, float max_slope_radians) noexcept {
    // Ground-slope limits are angles from +Z in [0, pi/2): a limit of 0 allows level ground only, and a limit that reaches a vertical
    // wall (pi/2) or beyond is not a slope limit, so nothing is walkable under it. A ceiling (normal.z < 0) is never ground.
    constexpr float half_pi = 1.57079632679f;
    if (!finite(normal) || !std::isfinite(max_slope_radians) || max_slope_radians < 0.0f || max_slope_radians >= half_pi) return false;
    if (!(normal.z > 0.0f)) return false;
    const double length2 = static_cast<double>(normal.x) * normal.x + static_cast<double>(normal.y) * normal.y +
                           static_cast<double>(normal.z) * normal.z;
    if (std::abs(length2 - 1.0) > 1e-3) return false;
    return normal.z >= std::cos(static_cast<double>(max_slope_radians)) - 1e-7;
}

} // namespace saige
