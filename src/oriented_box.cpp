#include "saige/oriented_box.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace saige {
namespace {

using D3 = std::array<double, 3>;

constexpr double tol = static_cast<double>(box_contact_tolerance);

D3 to_d3(Vec3 v) noexcept { return {v.x, v.y, v.z}; }
Vec3 to_vec3(const D3& v) noexcept { return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])}; }
D3 add(const D3& a, const D3& b) noexcept { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
D3 sub(const D3& a, const D3& b) noexcept { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
D3 scale(const D3& a, double s) noexcept { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const D3& a, const D3& b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D3 cross(const D3& a, const D3& b) noexcept { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double length(const D3& a) noexcept { return std::sqrt(dot(a, a)); }

bool finite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct Frame {
    D3 c;
    std::array<D3, 3> u;
    D3 h;
};

Frame frame_of(const OrientedBox& b) noexcept { return {to_d3(b.center), {to_d3(b.axis_x), to_d3(b.axis_y), to_d3(b.axis_z)}, to_d3(b.half_extents)}; }

// A convex primitive: its vertices, its edge directions, and (a triangle) its face normal.
struct Prim {
    std::array<D3, 8> v;
    int count = 0;
    std::array<D3, 3> edges;  // unit directions
    int edge_count = 0;
    D3 normal{0, 0, 0};
    bool has_normal = false;
    D3 center{0, 0, 0};
};

Prim prim_of(const Aabb& b) noexcept {
    Prim p;
    p.count = 8;
    for (int i = 0; i < 8; ++i) {
        p.v[static_cast<std::size_t>(i)] = {(i & 1) != 0 ? b.max.x : b.min.x, (i & 2) != 0 ? b.max.y : b.min.y, (i & 4) != 0 ? b.max.z : b.min.z};
    }
    p.edges = {D3{1, 0, 0}, D3{0, 1, 0}, D3{0, 0, 1}};
    p.edge_count = 3;
    p.center = {0.5 * (static_cast<double>(b.min.x) + b.max.x), 0.5 * (static_cast<double>(b.min.y) + b.max.y), 0.5 * (static_cast<double>(b.min.z) + b.max.z)};
    return p;
}

Prim prim_of(const Triangle& t) {
    Prim p;
    p.count = 3;
    p.v[0] = to_d3(t.a);
    p.v[1] = to_d3(t.b);
    p.v[2] = to_d3(t.c);
    const D3 e0 = sub(p.v[1], p.v[0]), e1 = sub(p.v[2], p.v[1]), e2 = sub(p.v[0], p.v[2]);
    const D3 n = cross(e0, sub(p.v[2], p.v[0]));
    const double nl = length(n);
    p.normal = scale(n, 1.0 / nl);
    p.has_normal = true;
    p.edges = {scale(e0, 1.0 / length(e0)), scale(e1, 1.0 / length(e1)), scale(e2, 1.0 / length(e2))};
    p.edge_count = 3;
    p.center = scale(add(add(p.v[0], p.v[1]), p.v[2]), 1.0 / 3.0);
    return p;
}

struct AxisGap {
    double gap;   // positive: separated along this axis; negative: overlapping by -gap
    D3 normal;    // unit, from the primitive toward the box
    D3 axis;      // the axis as generated (not sign-adjusted), for the swept test
    double b0, b1, p0, p1;
};

constexpr std::size_t max_axes = 15;

struct Sat {
    std::array<AxisGap, max_axes> axes;
    std::size_t count = 0;
    std::size_t best = 0;  // index of the largest gap, the lowest on a tie
};

void add_axis(Sat& sat, const Frame& f, const Prim& p, D3 axis) noexcept {
    const double len = length(axis);
    if (!(len > 1e-9)) return;  // a (near) zero cross product: the two edges are parallel and the axis adds nothing
    axis = scale(axis, 1.0 / len);
    const double cb = dot(axis, f.c);
    const double rb = f.h[0] * std::fabs(dot(axis, f.u[0])) + f.h[1] * std::fabs(dot(axis, f.u[1])) + f.h[2] * std::fabs(dot(axis, f.u[2]));
    double p0 = std::numeric_limits<double>::infinity(), p1 = -p0;
    for (int i = 0; i < p.count; ++i) {
        const double d = dot(axis, p.v[static_cast<std::size_t>(i)]);
        p0 = std::min(p0, d);
        p1 = std::max(p1, d);
    }
    const double b0 = cb - rb, b1 = cb + rb;
    AxisGap g;
    g.gap = std::max(p0 - b1, b0 - p1);
    g.normal = (b0 + b1) >= (p0 + p1) ? axis : scale(axis, -1.0);
    g.axis = axis;
    g.b0 = b0;
    g.b1 = b1;
    g.p0 = p0;
    g.p1 = p1;
    sat.axes[sat.count] = g;
    if (sat.count == 0 || g.gap > sat.axes[sat.best].gap) sat.best = sat.count;
    ++sat.count;
}

Sat sat_of(const Frame& f, const Prim& p) noexcept {
    Sat sat;
    if (p.has_normal) add_axis(sat, f, p, p.normal);
    for (int i = 0; i < 3 && !p.has_normal; ++i) add_axis(sat, f, p, p.edges[static_cast<std::size_t>(i)]);  // an AABB: its three face normals
    for (int j = 0; j < 3; ++j) add_axis(sat, f, p, f.u[static_cast<std::size_t>(j)]);
    for (int i = 0; i < p.edge_count; ++i) {
        for (int j = 0; j < 3; ++j) add_axis(sat, f, p, cross(p.edges[static_cast<std::size_t>(i)], f.u[static_cast<std::size_t>(j)]));
    }
    return sat;
}

struct Raw {
    double contact_t = 0.0;
    D3 normal{0, 0, 0};
    double penetration = 0.0;
    bool started = false;
};

std::optional<OrientedBoxOverlap> overlap_of(const Frame& f, const Prim& p) noexcept {
    const Sat sat = sat_of(f, p);
    const AxisGap& best = sat.axes[sat.best];
    if (!(best.gap < -tol)) return std::nullopt;
    return OrientedBoxOverlap{to_vec3(best.normal), static_cast<float>(-best.gap)};
}

std::optional<Raw> sweep_of(const Frame& f, const D3& d, const Prim& p) noexcept {
    const Sat sat = sat_of(f, p);
    const AxisGap& best = sat.axes[sat.best];
    Raw raw;
    if (best.gap < -tol) {  // already penetrating: the push-out direction, whatever the displacement
        raw.contact_t = 0.0;
        raw.normal = best.normal;
        raw.penetration = -best.gap;
        raw.started = true;
        return raw;
    }
    if (best.gap <= tol) {  // touching: a hit only when moving into the surface
        double most_into = std::numeric_limits<double>::infinity();
        D3 n{0, 0, 0};
        for (std::size_t i = 0; i < sat.count; ++i) {
            const AxisGap& a = sat.axes[i];
            if (a.gap > tol || a.gap < -tol) continue;
            const double into = dot(d, a.normal);
            if (into < most_into) {
                most_into = into;
                n = a.normal;
            }
        }
        if (!(most_into < -1e-12)) return std::nullopt;
        raw.contact_t = 0.0;
        raw.normal = n;
        raw.penetration = std::max(0.0, -best.gap);  // 0 to the tolerance: a shallow overlap is reported, but is not an 'initial overlap'
        return raw;
    }
    // Separated: the interval of times each axis's projections overlap.
    double enter = -std::numeric_limits<double>::infinity(), exit = std::numeric_limits<double>::infinity();
    D3 normal{0, 0, 0};
    for (std::size_t i = 0; i < sat.count; ++i) {
        const AxisGap& a = sat.axes[i];
        const double s = dot(a.axis, d);
        if (std::fabs(s) < 1e-15) {
            if (a.gap > 0.0) return std::nullopt;  // separated and not moving along this axis: never meets
            continue;
        }
        const double ta = (a.p0 - a.b1) / s, tb = (a.p1 - a.b0) / s;
        const double lo = std::min(ta, tb), hi = std::max(ta, tb);
        if (lo > enter) {
            enter = lo;
            normal = s > 0.0 ? scale(a.axis, -1.0) : a.axis;
        }
        exit = std::min(exit, hi);
        if (enter > exit || enter > 1.0 || exit < 0.0) return std::nullopt;
    }
    if (!(enter >= 0.0) || enter > exit || enter > 1.0) return std::nullopt;
    raw.contact_t = enter;
    raw.normal = normal;
    return raw;
}

double min_gap_to(const Frame& f, std::span<const Aabb> boxes, std::span<const Triangle> triangles) {
    double gap = std::numeric_limits<double>::infinity();
    for (const Aabb& b : boxes) {
        const Sat s = sat_of(f, prim_of(b));
        gap = std::min(gap, s.axes[s.best].gap);
    }
    for (const Triangle& t : triangles) {
        const Sat s = sat_of(f, prim_of(t));
        gap = std::min(gap, s.axes[s.best].gap);
    }
    return gap;
}

struct Stop {
    Vec3 position;
    double t;
    bool clear;  // the position is separated from (or exactly touching) every primitive given
};

Stop stop_of(const OrientedBox& box, Vec3 displacement, double contact_t, std::span<const Aabb> boxes, std::span<const Triangle> triangles) {
    const D3 d = to_d3(displacement);
    const D3 start = to_d3(box.center);
    if (!(contact_t > 0.0)) return {box.center, 0.0, min_gap_to(frame_of(box), boxes, triangles) >= 0.0};  // the start itself
    const double len = length(d);
    double t = std::min(contact_t, 1.0);
    double step = len > 0.0 ? std::ldexp(1.0, -17) / len : 0.0;  // 2^-17 m: well below the float coordinate step at the supported scale
    Frame f = frame_of(box);
    for (int k = 0; k < 48; ++k) {
        const Vec3 c = to_vec3(add(start, scale(d, t)));
        f.c = to_d3(c);
        if (min_gap_to(f, boxes, triangles) >= 0.0) return {c, t, true};
        t = std::max(0.0, t - step);
        step *= 2.0;
        if (t == 0.0) break;
    }
    return {box.center, 0.0, min_gap_to(frame_of(box), boxes, triangles) >= 0.0};  // gave up (never observed): the start, which may not be clear
}

OrientedBoxHit make_hit(const OrientedBox& box, Vec3 displacement, const Raw& raw, std::span<const Aabb> boxes, std::span<const Triangle> triangles) {
    OrientedBoxHit hit;
    hit.contact_t = raw.contact_t;
    hit.normal = to_vec3(raw.normal);
    hit.penetration = static_cast<float>(raw.penetration);
    hit.started_overlapping = raw.started;
    const Stop s = stop_of(box, displacement, raw.contact_t, boxes, triangles);
    hit.stop = s.position;
    hit.t = static_cast<float>(s.t);
    hit.stop_clear = s.clear;
    return hit;
}

bool valid_aabb(const Aabb& b) noexcept { return finite(b.min) && finite(b.max) && b.min.x < b.max.x && b.min.y < b.max.y && b.min.z < b.max.z; }

void check_primitives(std::span<const Aabb> boxes, std::span<const Triangle> triangles) {
    for (const Aabb& b : boxes) {
        if (!valid_aabb(b)) throw std::invalid_argument("OrientedBox query: invalid box");
    }
    for (const Triangle& t : triangles) {
        if (!is_valid_triangle(t)) throw std::invalid_argument("OrientedBox query: invalid triangle");
    }
}

} // namespace

OrientedBox make_oriented_box(Vec3 center, Vec3 half_extents, float yaw_radians) {
    if (!std::isfinite(yaw_radians)) throw std::invalid_argument("OrientedBox yaw must be finite");
    const double c = std::cos(static_cast<double>(yaw_radians)), s = std::sin(static_cast<double>(yaw_radians));
    OrientedBox box;
    box.center = center;
    box.half_extents = half_extents;
    box.axis_x = {static_cast<float>(c), static_cast<float>(s), 0.0f};
    box.axis_y = {static_cast<float>(-s), static_cast<float>(c), 0.0f};
    box.axis_z = {0.0f, 0.0f, 1.0f};
    validate_oriented_box(box);
    return box;
}

OrientedBox make_oriented_box(Vec3 center, Vec3 half_extents, float qx, float qy, float qz, float qw) {
    if (!std::isfinite(qx) || !std::isfinite(qy) || !std::isfinite(qz) || !std::isfinite(qw)) throw std::invalid_argument("OrientedBox quaternion must be finite");
    const double x = qx, y = qy, z = qz, w = qw;
    const double n = std::sqrt(x * x + y * y + z * z + w * w);
    if (!(n > 1e-12)) throw std::invalid_argument("OrientedBox quaternion must not be zero");
    const double a = x / n, b = y / n, c = z / n, d = w / n;
    OrientedBox box;
    box.center = center;
    box.half_extents = half_extents;
    box.axis_x = {static_cast<float>(1 - 2 * (b * b + c * c)), static_cast<float>(2 * (a * b + c * d)), static_cast<float>(2 * (a * c - b * d))};
    box.axis_y = {static_cast<float>(2 * (a * b - c * d)), static_cast<float>(1 - 2 * (a * a + c * c)), static_cast<float>(2 * (b * c + a * d))};
    box.axis_z = {static_cast<float>(2 * (a * c + b * d)), static_cast<float>(2 * (b * c - a * d)), static_cast<float>(1 - 2 * (a * a + b * b))};
    validate_oriented_box(box);
    return box;
}

void validate_oriented_box(const OrientedBox& box) {
    if (!finite(box.center) || !finite(box.half_extents) || !finite(box.axis_x) || !finite(box.axis_y) || !finite(box.axis_z)) {
        throw std::invalid_argument("OrientedBox values must be finite");
    }
    for (const float h : {box.half_extents.x, box.half_extents.y, box.half_extents.z}) {
        if (!(h > 0.0f) || h > max_box_half_extent) throw std::invalid_argument("OrientedBox half extents must be in (0, max_box_half_extent]");
    }
    const D3 u[3] = {to_d3(box.axis_x), to_d3(box.axis_y), to_d3(box.axis_z)};
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(length(u[i]) - 1.0) > 1e-4) throw std::invalid_argument("OrientedBox axes must be unit length");
        for (int j = i + 1; j < 3; ++j) {
            if (std::fabs(dot(u[i], u[j])) > 1e-4) throw std::invalid_argument("OrientedBox axes must be perpendicular");
        }
    }
}

void check_oriented_box_sweep_envelope(const OrientedBox& box, Vec3 displacement) {
    validate_oriented_box(box);
    if (!finite(displacement)) throw std::invalid_argument("OrientedBox displacement must be finite");
    const D3 d = to_d3(displacement);
    if (!(length(d) <= static_cast<double>(max_sweep_length))) throw std::invalid_argument("OrientedBox displacement exceeds max_sweep_length: subdivide the motion");
    const Frame f = frame_of(box);
    constexpr double extent = static_cast<double>(max_local_extent);
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const double reach = f.h[0] * std::fabs(f.u[0][axis]) + f.h[1] * std::fabs(f.u[1][axis]) + f.h[2] * std::fabs(f.u[2][axis]);
        const double lo = f.c[axis] - reach, hi = f.c[axis] + reach;
        if (std::min(lo, lo + d[axis]) < -extent || std::max(hi, hi + d[axis]) > extent) {
            throw std::invalid_argument("OrientedBox sweep leaves the supported +-max_local_extent range: rebase to chunk-local coordinates");
        }
    }
}

std::optional<OrientedBoxOverlap> oriented_box_aabb_overlap(const OrientedBox& box, const Aabb& aabb) {
    check_oriented_box_sweep_envelope(box, Vec3{0, 0, 0});
    check_primitives(std::span<const Aabb>(&aabb, 1), {});
    return overlap_of(frame_of(box), prim_of(aabb));
}

std::optional<OrientedBoxOverlap> oriented_box_triangle_overlap(const OrientedBox& box, const Triangle& triangle) {
    check_oriented_box_sweep_envelope(box, Vec3{0, 0, 0});
    check_primitives({}, std::span<const Triangle>(&triangle, 1));
    return overlap_of(frame_of(box), prim_of(triangle));
}

double oriented_box_aabb_gap(const OrientedBox& box, const Aabb& aabb) {
    check_oriented_box_sweep_envelope(box, Vec3{0, 0, 0});
    check_primitives(std::span<const Aabb>(&aabb, 1), {});
    const Sat s = sat_of(frame_of(box), prim_of(aabb));
    return s.axes[s.best].gap;
}

double oriented_box_triangle_gap(const OrientedBox& box, const Triangle& triangle) {
    check_oriented_box_sweep_envelope(box, Vec3{0, 0, 0});
    check_primitives({}, std::span<const Triangle>(&triangle, 1));
    const Sat s = sat_of(frame_of(box), prim_of(triangle));
    return s.axes[s.best].gap;
}

std::optional<OrientedBoxHit> sweep_oriented_box_aabb(const OrientedBox& box, Vec3 displacement, const Aabb& aabb) {
    return sweep_oriented_box(box, displacement, std::span<const Aabb>(&aabb, 1), {});
}

std::optional<OrientedBoxHit> sweep_oriented_box_triangle(const OrientedBox& box, Vec3 displacement, const Triangle& triangle) {
    return sweep_oriented_box(box, displacement, {}, std::span<const Triangle>(&triangle, 1));
}

std::optional<OrientedBoxHit> sweep_oriented_box(const OrientedBox& box, Vec3 displacement, std::span<const Aabb> boxes, std::span<const Triangle> triangles) {
    check_oriented_box_sweep_envelope(box, displacement);
    check_primitives(boxes, triangles);
    const Frame f = frame_of(box);
    const D3 d = to_d3(displacement);
    std::optional<Raw> best;
    std::size_t best_index = 0;
    bool best_is_triangle = false;
    const auto consider = [&](const std::optional<Raw>& raw, std::size_t index, bool is_triangle) {
        if (!raw) return;
        // Earliest; at an exact tie the deeper initial overlap, then the earlier primitive (boxes were examined first, so a triangle must be strictly better).
        const bool better = !best || raw->contact_t < best->contact_t || (raw->contact_t == best->contact_t && raw->penetration > best->penetration);
        if (better) {
            best = raw;
            best_index = index;
            best_is_triangle = is_triangle;
        }
    };
    for (std::size_t i = 0; i < boxes.size(); ++i) consider(sweep_of(f, d, prim_of(boxes[i])), i, false);
    for (std::size_t i = 0; i < triangles.size(); ++i) consider(sweep_of(f, d, prim_of(triangles[i])), i, true);
    if (!best) return std::nullopt;
    OrientedBoxHit hit = make_hit(box, displacement, *best, boxes, triangles);
    hit.index = best_index;
    hit.on_triangle = best_is_triangle;
    return hit;
}

Vec3 oriented_box_stop(const OrientedBox& box, Vec3 displacement, double contact_t, std::span<const Aabb> boxes, std::span<const Triangle> triangles) {
    check_oriented_box_sweep_envelope(box, displacement);
    check_primitives(boxes, triangles);
    if (!std::isfinite(contact_t)) throw std::invalid_argument("oriented_box_stop: the contact fraction must be finite");
    return stop_of(box, displacement, contact_t, boxes, triangles).position;
}

} // namespace saige
