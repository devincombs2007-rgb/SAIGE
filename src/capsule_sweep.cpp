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
    const double radius = capsule.radius;
    const double bottom = static_cast<double>(capsule.feet.z) + radius;
    const double top = static_cast<double>(capsule.feet.z) + capsule.height - radius;
    // A vertical segment and a box are separable per axis: x and y distances, and the gap between the z intervals.
    const double dx = std::max({static_cast<double>(box.min.x) - capsule.feet.x, 0.0, static_cast<double>(capsule.feet.x) - box.max.x});
    const double dy = std::max({static_cast<double>(box.min.y) - capsule.feet.y, 0.0, static_cast<double>(capsule.feet.y) - box.max.y});
    const double dz = std::max({static_cast<double>(box.min.z) - top, 0.0, bottom - static_cast<double>(box.max.z)});
    return static_cast<float>(std::sqrt(dx * dx + dy * dy + dz * dz) - radius);
}

std::optional<CapsuleHit> sweep_capsule_box(const Capsule& capsule, Vec3 displacement, const Aabb& box) {
    validate(capsule, box);
    if (!finite(displacement)) throw std::invalid_argument("Capsule displacement must be finite");

    const double r = capsule.radius;
    const double segment = static_cast<double>(capsule.height) - 2.0 * r;
    const D3 start{capsule.feet.x, capsule.feet.y, static_cast<double>(capsule.feet.z) + r};  // lowest sphere center
    const D3 d = to_d3(displacement);

    // Moving the whole capsule equals moving its lowest sphere center against the box grown downward by the segment length.
    const D3 lo{box.min.x, box.min.y, static_cast<double>(box.min.z) - segment};
    const D3 hi{box.max.x, box.max.y, box.max.z};


    const auto make_hit = [&](double t, const D3& normal, bool overlapping) {
        CapsuleHit hit;
        hit.t = static_cast<float>(t);
        hit.normal = to_vec3(normal);
        hit.point = to_vec3(axis_contact_point({start[0] + t * d[0], start[1] + t * d[1], start[2] + t * d[2]}, segment, box));
        hit.started_overlapping = overlapping;
        return hit;
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

std::optional<CapsuleHit> sweep_capsule_boxes(const Capsule& capsule, Vec3 displacement, std::span<const Aabb> boxes) {
    // The query is validated once, up front, so an invalid capsule or displacement throws whatever the set holds (an empty set
    // included); each box is validated by the single-box sweep.
    validate(capsule);
    if (!finite(displacement)) throw std::invalid_argument("Capsule displacement must be finite");
    std::optional<CapsuleHit> best;
    for (std::size_t i = 0; i < boxes.size(); ++i) {
        auto hit = sweep_capsule_box(capsule, displacement, boxes[i]);
        if (hit && (!best || hit->t < best->t)) {
            hit->index = i;
            best = hit;
        }
    }
    return best;
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
