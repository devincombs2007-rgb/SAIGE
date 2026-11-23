#include "saige/capsule_triangle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace saige {
namespace {

using D3 = std::array<double, 3>;

constexpr double touch_tolerance = 1e-5;   // the contact tolerance shared with the box sweep and the character
constexpr double converge_gap = 1e-8;      // conservative advancement stops once the gap is this small
constexpr int max_iterations = 96;
constexpr double min_cross_length = 1e-10; // twice the smallest accepted triangle area, square meters

D3 to_d3(Vec3 v) noexcept { return {v.x, v.y, v.z}; }
Vec3 to_vec3(const D3& v) noexcept { return {static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2])}; }
D3 add(const D3& a, const D3& b) noexcept { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
D3 sub(const D3& a, const D3& b) noexcept { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
D3 mul(const D3& a, double s) noexcept { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const D3& a, const D3& b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D3 cross(const D3& a, const D3& b) noexcept { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double length(const D3& a) noexcept { return std::sqrt(dot(a, a)); }

bool finite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct Tri {
    D3 a, b, c;
    D3 normal;  // unit, (b - a) x (c - a)
};

bool make_tri(const Triangle& t, Tri& out) noexcept {
    if (!finite(t.a) || !finite(t.b) || !finite(t.c)) return false;
    out.a = to_d3(t.a);
    out.b = to_d3(t.b);
    out.c = to_d3(t.c);
    const D3 n = cross(sub(out.b, out.a), sub(out.c, out.a));
    const double len = length(n);
    if (!(len > min_cross_length)) return false;
    out.normal = mul(n, 1.0 / len);
    return true;
}

Tri require_tri(const Triangle& t) {
    Tri out;
    if (!make_tri(t, out)) throw std::invalid_argument("Triangle must be finite and not degenerate");
    return out;
}

void validate(const Capsule& capsule) {
    if (!finite(capsule.feet) || !std::isfinite(capsule.radius) || !std::isfinite(capsule.height) || capsule.radius <= 0.0f ||
        capsule.height < 2.0f * capsule.radius) {
        throw std::invalid_argument("Capsule must be finite with a positive radius and a height of at least twice the radius");
    }
}

struct Core {
    D3 p0, p1;  // the vertical core segment, bottom and top
    double radius;
};

Core core_at(const Capsule& capsule, const D3& displacement, double t) noexcept {
    const double r = capsule.radius;
    const D3 feet = add(to_d3(capsule.feet), mul(displacement, t));
    return {{feet[0], feet[1], feet[2] + r}, {feet[0], feet[1], feet[2] + (static_cast<double>(capsule.height) - r)}, r};
}

// Closest point of triangle abc to p (Ericson, Real-Time Collision Detection 5.1.5).
D3 closest_on_triangle(const D3& p, const D3& a, const D3& b, const D3& c) noexcept {
    const D3 ab = sub(b, a), ac = sub(c, a), ap = sub(p, a);
    const double d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) return a;
    const D3 bp = sub(p, b);
    const double d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) return b;
    const double vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) return add(a, mul(ab, d1 / (d1 - d3)));
    const D3 cp = sub(p, c);
    const double d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) return c;
    const double vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) return add(a, mul(ac, d2 / (d2 - d6)));
    const double va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) return add(b, mul(sub(c, b), (d4 - d3) / ((d4 - d3) + (d5 - d6))));
    const double denom = 1.0 / (va + vb + vc);
    return add(a, add(mul(ab, vb * denom), mul(ac, vc * denom)));
}

// Closest points between segments p1-q1 and p2-q2 (Ericson 5.1.9).
void closest_segment_segment(const D3& p1, const D3& q1, const D3& p2, const D3& q2, D3& c1, D3& c2) noexcept {
    const D3 d1 = sub(q1, p1), d2 = sub(q2, p2), r = sub(p1, p2);
    const double a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    constexpr double eps = 1e-18;
    double s = 0.0, t = 0.0;
    if (a <= eps && e <= eps) {
        s = t = 0.0;
    } else if (a <= eps) {
        t = std::clamp(f / e, 0.0, 1.0);
    } else {
        const double c = dot(d1, r);
        if (e <= eps) {
            s = std::clamp(-c / a, 0.0, 1.0);
        } else {
            const double b = dot(d1, d2);
            const double denom = a * e - b * b;
            s = denom > 0.0 ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0) {
                t = 0.0;
                s = std::clamp(-c / a, 0.0, 1.0);
            } else if (t > 1.0) {
                t = 1.0;
                s = std::clamp((b - c) / a, 0.0, 1.0);
            }
        }
    }
    c1 = add(p1, mul(d1, s));
    c2 = add(p2, mul(d2, t));
}

struct Closest {
    double distance = 0.0;  // between the core segment and the triangle; 0 when the segment crosses the triangle
    D3 on_core{};
    D3 on_triangle{};
    bool crosses = false;   // the core segment passes through the triangle's face
};

Closest closest_core_triangle(const Core& core, const Tri& tri) noexcept {
    Closest best;
    // The segment crosses the face: its ends are on opposite sides of (or on) the plane, and the crossing point is inside.
    const double d0 = dot(tri.normal, sub(core.p0, tri.a));
    const double d1 = dot(tri.normal, sub(core.p1, tri.a));
    if ((d0 <= 0.0 && d1 >= 0.0) || (d0 >= 0.0 && d1 <= 0.0)) {
        if (d0 != d1) {
            const D3 x = add(core.p0, mul(sub(core.p1, core.p0), d0 / (d0 - d1)));
            const D3 q = closest_on_triangle(x, tri.a, tri.b, tri.c);
            if (length(sub(x, q)) <= 1e-12) {
                best.distance = 0.0;
                best.on_core = x;
                best.on_triangle = x;
                best.crosses = true;
                return best;
            }
        }
    }
    best.distance = std::numeric_limits<double>::infinity();
    const auto consider = [&](const D3& on_core, const D3& on_triangle) {
        const double d = length(sub(on_core, on_triangle));
        if (d < best.distance) {
            best.distance = d;
            best.on_core = on_core;
            best.on_triangle = on_triangle;
        }
    };
    consider(core.p0, closest_on_triangle(core.p0, tri.a, tri.b, tri.c));
    consider(core.p1, closest_on_triangle(core.p1, tri.a, tri.b, tri.c));
    const std::array<std::array<const D3*, 2>, 3> edges{{{&tri.a, &tri.b}, {&tri.b, &tri.c}, {&tri.c, &tri.a}}};
    for (const auto& edge : edges) {
        D3 on_core{}, on_edge{};
        closest_segment_segment(core.p0, core.p1, *edge[0], *edge[1], on_core, on_edge);
        consider(on_core, on_edge);
    }
    return best;
}

// Unit direction from the triangle toward the core at the closest points; for a crossing core, the face normal toward the side
// the core's midpoint is on (the +normal side on a tie).
D3 separating_direction(const Closest& c, const Core& core, const Tri& tri) noexcept {
    if (!c.crosses && c.distance > 1e-12) return mul(sub(c.on_core, c.on_triangle), 1.0 / c.distance);
    const D3 mid = mul(add(core.p0, core.p1), 0.5);
    return dot(tri.normal, sub(mid, tri.a)) >= 0.0 ? tri.normal : mul(tri.normal, -1.0);
}

// Translation that just separates an overlapping capsule (gap < 0) from the triangle, or nullopt (and a zero direction) if not
// overlapping within `tolerance`.
std::optional<D3> push_out(const Core& core, const Tri& tri, double tolerance) noexcept {
    const Closest c = closest_core_triangle(core, tri);
    if (!c.crosses) {
        if (c.distance >= core.radius - tolerance) return std::nullopt;
        const D3 n = separating_direction(c, core, tri);
        return mul(n, core.radius - c.distance);
    }
    // The core crosses the face: leave through the nearer side of the plane.
    const double d0 = dot(tri.normal, sub(core.p0, tri.a));
    const double d1 = dot(tri.normal, sub(core.p1, tri.a));
    const double up = core.radius - std::min(d0, d1);    // push toward +normal until the whole core is a radius above the plane
    const double down = core.radius + std::max(d0, d1);  // or toward -normal
    return up <= down ? mul(tri.normal, up) : mul(tri.normal, -down);
}

struct Bounds {
    D3 lo, hi;
};

bool bounds_overlap(const Bounds& a, const Bounds& b) noexcept {
    return a.lo[0] <= b.hi[0] && a.hi[0] >= b.lo[0] && a.lo[1] <= b.hi[1] && a.hi[1] >= b.lo[1] && a.lo[2] <= b.hi[2] && a.hi[2] >= b.lo[2];
}

Bounds swept_bounds(const Capsule& capsule, const D3& displacement) noexcept {
    const D3 f0 = to_d3(capsule.feet);
    const D3 f1 = add(f0, displacement);
    const double r = static_cast<double>(capsule.radius) + 1e-4;
    return {{std::min(f0[0], f1[0]) - r, std::min(f0[1], f1[1]) - r, std::min(f0[2], f1[2]) - 1e-4},
            {std::max(f0[0], f1[0]) + r, std::max(f0[1], f1[1]) + r, std::max(f0[2], f1[2]) + static_cast<double>(capsule.height) + 1e-4}};
}

Bounds triangle_bounds(const Tri& t) noexcept {
    Bounds b{t.a, t.a};
    for (const D3* p : {&t.b, &t.c}) {
        for (int i = 0; i < 3; ++i) {
            b.lo[static_cast<std::size_t>(i)] = std::min(b.lo[static_cast<std::size_t>(i)], (*p)[static_cast<std::size_t>(i)]);
            b.hi[static_cast<std::size_t>(i)] = std::max(b.hi[static_cast<std::size_t>(i)], (*p)[static_cast<std::size_t>(i)]);
        }
    }
    return b;
}

struct RawHit {
    CapsuleHit hit;  // t is the float of exact_t; stop is not set yet
    double exact_t{};
};

RawHit make_hit(double t, const D3& normal, const D3& point, bool overlapping, bool converged = true) noexcept {
    RawHit raw;
    raw.exact_t = std::clamp(t, 0.0, 1.0);
    raw.hit.t = static_cast<float>(raw.exact_t);
    raw.hit.normal = to_vec3(normal);
    raw.hit.point = to_vec3(point);
    raw.hit.started_overlapping = overlapping;
    raw.hit.converged = converged;
    return raw;
}

// Signed gap for a capsule whose feet are `feet` (the pose is judged in double, exactly as the caller will hold it in float).
double triangle_gap(const Capsule& capsule, Vec3 feet, const Tri& tri) noexcept {
    Capsule posed = capsule;
    posed.feet = feet;
    const Core core = core_at(posed, {0, 0, 0}, 0.0);
    const Closest c = closest_core_triangle(core, tri);
    return c.crosses ? -core.radius : c.distance - core.radius;
}

std::optional<RawHit> sweep_validated(const Capsule& capsule, const D3& displacement, const Tri& tri) noexcept {
    const double speed = length(displacement);
    const double r = capsule.radius;
    {   // Overlap at the start.
        const Core core = core_at(capsule, displacement, 0.0);
        if (const auto push = push_out(core, tri, touch_tolerance)) {
            const double len = length(*push);
            const D3 n = len > 0.0 ? mul(*push, 1.0 / len) : tri.normal;
            return make_hit(0.0, n, closest_core_triangle(core, tri).on_triangle, true);
        }
    }
    double t = 0.0;
    for (int iteration = 0; iteration < max_iterations; ++iteration) {
        const Core core = core_at(capsule, displacement, t);
        const Closest c = closest_core_triangle(core, tri);
        const double gap = c.distance - r;
        const D3 n = separating_direction(c, core, tri);
        const double closing = -dot(displacement, n);  // rate at which the gap shrinks per unit t
        // The distance between two convex shapes is a convex function of a translation. If it is not shrinking here, it never shrinks
        // again, and before here it was larger than the radius: no hit. (This is also the touching rule: a capsule that touches but
        // moves away or along the surface does not hit it.)
        if (!(closing > 1e-12 * std::max(1.0, speed))) return std::nullopt;
        if (gap <= converge_gap * 4.0 || (t == 0.0 && gap <= touch_tolerance)) return make_hit(t, n, c.on_triangle, false);
        const double next = t + (gap - 0.5 * converge_gap) / closing;
        if (next > 1.0) return std::nullopt;  // rigorous: the gap cannot close before t = next
        t = next;
    }
    // Out of iterations. Conservative advancement never passes the contact, so t is a clear position, but the gap there did not close
    // to the tolerance: report it as a hit that did not converge (a stop short of the contact) rather than as a contact.
    const Core core = core_at(capsule, displacement, t);
    const Closest c = closest_core_triangle(core, tri);
    return make_hit(t, separating_direction(c, core, tri), c.on_triangle, false, false);
}

// Fixes `raw.hit.stop` and `raw.hit.t` (see detail::conservative_stop). The triangle that was hit is held to its start gap exactly; the
// other triangles near the path to half the contact tolerance, so a rounding step along a touched surface does not block the motion.
CapsuleHit finalize(const Capsule& capsule, Vec3 displacement, RawHit raw, std::span<const Triangle> triangles, std::size_t hit_index) {
    CapsuleHit hit = raw.hit;
    if (hit.started_overlapping || raw.exact_t <= 0.0) {
        hit.t = 0.0f;
        hit.stop = capsule.feet;
        return hit;
    }
    const Bounds near_path = swept_bounds(capsule, to_d3(displacement));
    const auto clear = [&](Vec3 pose) {
        for (std::size_t i = 0; i < triangles.size(); ++i) {
            Tri tri;
            if (!make_tri(triangles[i], tri) || !bounds_overlap(near_path, triangle_bounds(tri))) continue;
            const double allowed = std::min(triangle_gap(capsule, capsule.feet, tri), 0.0) - (i == hit_index ? 0.0 : 0.5 * touch_tolerance);
            if (triangle_gap(capsule, pose, tri) < allowed) return false;
        }
        return true;
    };
    hit.stop = detail::conservative_stop(capsule.feet, displacement, raw.exact_t, clear);
    const D3 d = to_d3(displacement);
    const double along = dot(d, d) > 0.0 ? dot(sub(to_d3(hit.stop), to_d3(capsule.feet)), d) / dot(d, d) : 0.0;
    hit.t = static_cast<float>(std::clamp(along, 0.0, raw.exact_t));
    return hit;
}

} // namespace

bool is_valid_triangle(const Triangle& triangle) noexcept {
    Tri t;
    return make_tri(triangle, t);
}

Vec3 triangle_normal(const Triangle& triangle) {
    return to_vec3(require_tri(triangle).normal);
}

float capsule_triangle_gap(const Capsule& capsule, const Triangle& triangle) {
    validate(capsule);
    const Tri tri = require_tri(triangle);
    const Core core = core_at(capsule, {0, 0, 0}, 0.0);
    const Closest c = closest_core_triangle(core, tri);
    return static_cast<float>(c.crosses ? -core.radius : c.distance - core.radius);
}

std::optional<CapsuleHit> sweep_capsule_triangle(const Capsule& capsule, Vec3 displacement, const Triangle& triangle) {
    validate(capsule);
    check_sweep_envelope(capsule, displacement);
    const Tri tri = require_tri(triangle);
    const auto raw = sweep_validated(capsule, to_d3(displacement), tri);
    if (!raw) return std::nullopt;
    return finalize(capsule, displacement, *raw, std::span<const Triangle>(&triangle, 1), 0);
}

std::optional<CapsuleHit> sweep_capsule_triangles(const Capsule& capsule, Vec3 displacement, std::span<const Triangle> triangles) {
    validate(capsule);
    check_sweep_envelope(capsule, displacement);
    const D3 d = to_d3(displacement);
    const Bounds sweep_box = swept_bounds(capsule, d);
    std::optional<RawHit> best;
    std::size_t best_index = 0;
    for (std::size_t i = 0; i < triangles.size(); ++i) {
        const Tri tri = require_tri(triangles[i]);
        if (!bounds_overlap(sweep_box, triangle_bounds(tri))) continue;
        if (auto raw = sweep_validated(capsule, d, tri)) {
            if (!best || raw->hit.t < best->hit.t) {  // ties go to the lowest index
                best = raw;
                best_index = i;
            }
        }
    }
    if (!best) return std::nullopt;
    CapsuleHit hit = finalize(capsule, displacement, *best, triangles, best_index);
    hit.index = best_index;
    return hit;
}

std::optional<Vec3> capsule_triangle_push_out(const Capsule& capsule, const Triangle& triangle) {
    validate(capsule);
    check_sweep_envelope(capsule, Vec3{0.0f, 0.0f, 0.0f});
    const Tri tri = require_tri(triangle);
    const Core core = core_at(capsule, {0, 0, 0}, 0.0);
    if (const auto push = push_out(core, tri, touch_tolerance)) return to_vec3(*push);
    return std::nullopt;
}

} // namespace saige
