#include "saige/oriented_box.hpp"
#include "saige/static_collision.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using saige::Aabb;
using saige::OrientedBox;
using saige::StaticCollision;
using saige::StaticPrimitive;
using saige::Triangle;
using saige::Vec3;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

struct Random {
    std::uint64_t state;
    explicit Random(std::uint64_t seed) : state(seed) {}
    double next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state >> 11) / 9007199254740992.0;
    }
    float range(double lo, double hi) { return static_cast<float>(lo + (hi - lo) * next()); }
};

template <class F>
bool throws_invalid(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

OrientedBox random_box(Random& r, double extent) {
    const Vec3 h{r.range(0.3, 1.6), r.range(0.3, 1.0), r.range(0.2, 0.8)};
    float q[4] = {r.range(-1, 1), r.range(-1, 1), r.range(-1, 1), r.range(-1, 1)};
    if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] < 0.01f) q[3] = 1.0f;
    return saige::make_oriented_box({r.range(-extent, extent), r.range(-extent, extent), r.range(-extent * 0.3, extent * 0.3)}, h, q[0], q[1], q[2], q[3]);
}

// ---------------------------------------------------------------------------------------------------------------------------
// An oracle that shares nothing with the separating-axis code: do two solids intersect? Vertex containment plus edge pierce tests (two convex
// polyhedra intersect exactly when a vertex of one lies in the other or an edge of one meets the other).

using D3 = std::array<double, 3>;
D3 d3(Vec3 v) { return {v.x, v.y, v.z}; }
D3 sub(D3 a, D3 b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
D3 add(D3 a, D3 b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
D3 mul(D3 a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(D3 a, D3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D3 cross(D3 a, D3 b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }

struct Obb {
    D3 c, u[3], h;
};
Obb obb(const OrientedBox& b, D3 center) { return {center, {d3(b.axis_x), d3(b.axis_y), d3(b.axis_z)}, d3(b.half_extents)}; }

std::array<D3, 8> obb_vertices(const Obb& o) {
    std::array<D3, 8> v;
    for (int i = 0; i < 8; ++i) {
        D3 p = o.c;
        for (int k = 0; k < 3; ++k) p = add(p, mul(o.u[k], ((i >> k) & 1) != 0 ? o.h[static_cast<std::size_t>(k)] : -o.h[static_cast<std::size_t>(k)]));
        v[static_cast<std::size_t>(i)] = p;
    }
    return v;
}

bool inside(const Obb& o, D3 p, double slack = 1e-12) {
    const D3 d = sub(p, o.c);
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(dot(d, o.u[k])) > o.h[static_cast<std::size_t>(k)] + slack) return false;
    }
    return true;
}

// Segment p-q against the solid box: slab test in the box frame.
bool segment_hits(const Obb& o, D3 p, D3 q) {
    double t0 = 0.0, t1 = 1.0;
    for (int k = 0; k < 3; ++k) {
        const double a = dot(sub(p, o.c), o.u[k]), b = dot(sub(q, o.c), o.u[k]), h = o.h[static_cast<std::size_t>(k)];
        const double dir = b - a;
        if (std::fabs(dir) < 1e-14) {
            if (std::fabs(a) > h) return false;
            continue;
        }
        double ta = (-h - a) / dir, tb = (h - a) / dir;
        if (ta > tb) std::swap(ta, tb);
        t0 = std::max(t0, ta);
        t1 = std::min(t1, tb);
        if (t0 > t1) return false;
    }
    return true;
}

const int cube_edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};

bool segment_triangle(D3 p, D3 q, const Triangle& t) {  // Moller-Trumbore on the closed segment, closed triangle
    const D3 a = d3(t.a), e1 = sub(d3(t.b), a), e2 = sub(d3(t.c), a), dir = sub(q, p);
    const D3 pv = cross(dir, e2);
    const double det = dot(e1, pv);
    if (std::fabs(det) < 1e-14) return false;
    const double inv = 1.0 / det;
    const D3 tv = sub(p, a);
    const double u = dot(tv, pv) * inv;
    if (u < -1e-12 || u > 1.0 + 1e-12) return false;
    const D3 qv = cross(tv, e1);
    const double v = dot(dir, qv) * inv;
    if (v < -1e-12 || u + v > 1.0 + 1e-12) return false;
    const double s = dot(e2, qv) * inv;
    return s >= -1e-12 && s <= 1.0 + 1e-12;
}

Obb aabb_as_obb(const Aabb& a) {
    return {{0.5 * (a.min.x + double{a.max.x}), 0.5 * (a.min.y + double{a.max.y}), 0.5 * (a.min.z + double{a.max.z})},
            {D3{1, 0, 0}, D3{0, 1, 0}, D3{0, 0, 1}},
            {0.5 * (a.max.x - double{a.min.x}), 0.5 * (a.max.y - double{a.min.y}), 0.5 * (a.max.z - double{a.min.z})}};
}

bool solids_intersect(const Obb& a, const Obb& b) {
    const auto va = obb_vertices(a), vb = obb_vertices(b);
    for (const D3& p : va) {
        if (inside(b, p)) return true;
    }
    for (const D3& p : vb) {
        if (inside(a, p)) return true;
    }
    for (const auto& e : cube_edges) {
        if (segment_hits(b, va[static_cast<std::size_t>(e[0])], va[static_cast<std::size_t>(e[1])])) return true;
        if (segment_hits(a, vb[static_cast<std::size_t>(e[0])], vb[static_cast<std::size_t>(e[1])])) return true;
    }
    return false;
}

bool box_hits_triangle(const Obb& o, const Triangle& t) {
    const D3 v[3] = {d3(t.a), d3(t.b), d3(t.c)};
    for (const D3& p : v) {
        if (inside(o, p)) return true;
    }
    for (int i = 0; i < 3; ++i) {
        if (segment_hits(o, v[i], v[(i + 1) % 3])) return true;
    }
    const auto ov = obb_vertices(o);
    for (const auto& e : cube_edges) {
        if (segment_triangle(ov[static_cast<std::size_t>(e[0])], ov[static_cast<std::size_t>(e[1])], t)) return true;
    }
    return false;
}

// A stop must be clear, not merely within the contact tolerance: its separation (a lower bound on the distance, from the library's own SAT, whose sign is
// checked against the independent oracle by the overlap tests) is not negative.
bool strictly_clear_of_aabb(const OrientedBox& b, D3 center, const Aabb& a) {
    OrientedBox moved = b;
    moved.center = {static_cast<float>(center[0]), static_cast<float>(center[1]), static_cast<float>(center[2])};
    return saige::oriented_box_aabb_gap(moved, a) >= 0.0;
}
bool strictly_clear_of_triangle(const OrientedBox& b, D3 center, const Triangle& t) {
    OrientedBox moved = b;
    moved.center = {static_cast<float>(center[0]), static_cast<float>(center[1]), static_cast<float>(center[2])};
    return saige::oriented_box_triangle_gap(moved, t) >= 0.0;
}

// First time in [0, 1] the moving box touches the primitive, by sampling then bisection on the independent predicate; nothing if it never does.
template <class Hit>
std::optional<double> oracle_toi(const OrientedBox& box, Vec3 d, Hit&& intersects) {
    const D3 c0 = d3(box.center), dd = d3(d);
    const auto at = [&](double t) { return intersects(obb(box, add(c0, mul(dd, t)))); };
    if (at(0.0)) return 0.0;
    constexpr int steps = 4000;
    double previous = 0.0;
    for (int i = 1; i <= steps; ++i) {
        const double t = static_cast<double>(i) / steps;
        if (at(t)) {
            double lo = previous, hi = t;
            for (int k = 0; k < 60; ++k) {
                const double mid = 0.5 * (lo + hi);
                (at(mid) ? hi : lo) = mid;
            }
            return hi;
        }
        previous = t;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------------------------------------

void test_analytic_cases() {
    const Aabb wall{{4.0f, -5.0f, -5.0f}, {5.0f, 5.0f, 5.0f}};
    // An axis-aligned box moving +X into the wall's face.
    {
        const OrientedBox box = saige::make_oriented_box({0, 0, 0}, {1.0f, 0.5f, 0.5f}, 0.0f);
        const auto hit = saige::sweep_oriented_box_aabb(box, {5.0f, 0, 0}, wall);
        require(hit.has_value(), "axis aligned: hit");
        require(near(hit->contact_t, 0.6, 1e-12), "axis aligned: contact at 3 m of 5 (the box face reaches x = 4): " + std::to_string(hit->contact_t));
        require(near(hit->normal.x, -1.0, 1e-6) && near(hit->normal.y, 0.0, 1e-6) && near(hit->normal.z, 0.0, 1e-6), "axis aligned: normal faces the box");
        require(!hit->started_overlapping && hit->penetration == 0.0f, "axis aligned: not an initial overlap");
        require(hit->stop.x <= 3.0f && hit->stop.x > 2.999f, "axis aligned: the stop is just short of the contact: " + std::to_string(hit->stop.x));
        require(near(hit->t, hit->stop.x / 5.0, 1e-6), "axis aligned: t matches the stop");
        require(!saige::oriented_box_aabb_overlap(saige::make_oriented_box(hit->stop, {1.0f, 0.5f, 0.5f}, 0.0f), wall).has_value(), "the stop is clear");
    }
    // The same box turned 45 degrees about Z: its corner leads, reaching the wall after 4 - sqrt(0.5 * 1.0^2 + 0.5 * 0.5^2)... computed from the frame.
    {
        const float yaw = 0.785398163f;
        const OrientedBox box = saige::make_oriented_box({0, 0, 0}, {1.0f, 0.5f, 0.5f}, yaw);
        const double reach = std::fabs(box.axis_x.x) * 1.0 + std::fabs(box.axis_y.x) * 0.5;  // support in +X of the turned box
        const auto hit = saige::sweep_oriented_box_aabb(box, {6.0f, 0, 0}, wall);
        require(hit.has_value(), "rotated: hit");
        require(near(hit->contact_t, (4.0 - reach) / 6.0, 1e-7), "rotated: the corner reaches the wall at (4 - support) / 6: " + std::to_string(hit->contact_t));
        require(near(hit->normal.x, -1.0, 1e-6), "rotated: the wall's face normal");
    }
    // A box moving past the wall without reaching it, and one moving away.
    {
        const OrientedBox box = saige::make_oriented_box({0, 0, 0}, {1.0f, 0.5f, 0.5f}, 0.0f);
        require(!saige::sweep_oriented_box_aabb(box, {2.0f, 0, 0}, wall).has_value(), "short of the wall: no hit");
        require(!saige::sweep_oriented_box_aabb(box, {-6.0f, 0, 0}, wall).has_value(), "moving away: no hit");
        require(!saige::sweep_oriented_box_aabb(box, {0, 0, 9.0f}, wall).has_value(), "moving parallel to the wall, clear of it: no hit");
    }
    // A ramp triangle: a box sliding down onto it hits at the contact the plane gives.
    {
        const Triangle ramp{{-5, -5, 0}, {5, -5, 0}, {0, 5, 0}};  // the plane z = 0
        const OrientedBox box = saige::make_oriented_box({0, 0, 3.0f}, {0.5f, 0.5f, 0.5f}, 0.3f);
        const auto hit = saige::sweep_oriented_box_triangle(box, {0, 0, -4.0f}, ramp);
        require(hit.has_value() && near(hit->contact_t, 2.5 / 4.0, 1e-9), "triangle: lands at z = 0.5 above the plane");
        require(near(hit->normal.z, 1.0, 1e-6), "triangle: the normal points up from the plane");
        require(saige::sweep_oriented_box_triangle(box, {0, 0, -2.0f}, ramp).has_value() == false, "triangle: short of the plane");
        // Coming from below, the (two-sided) triangle's normal points down, toward the box.
        const OrientedBox below = saige::make_oriented_box({0, 0, -3.0f}, {0.5f, 0.5f, 0.5f}, 0.0f);
        const auto up = saige::sweep_oriented_box_triangle(below, {0, 0, 4.0f}, ramp);
        require(up.has_value() && near(up->normal.z, -1.0, 1e-6), "triangle: two sided");
    }
    // A pure edge-edge contact (the crossing-edges axis decides): a box tilted so that one edge leads into a wall's edge.
    {
        const Aabb post{{2.0f, -0.5f, -10.0f}, {3.0f, 0.5f, 10.0f}};
        const OrientedBox box = saige::make_oriented_box({0, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.785398163f);  // a diamond seen from above, a vertical edge leads in +X
        const auto hit = saige::sweep_oriented_box_aabb(box, {5.0f, 0, 0}, post);
        require(hit.has_value() && near(hit->contact_t, (2.0 - 0.5 * std::sqrt(2.0)) / 5.0, 1e-6), "edge leading into a face: the face axis");
    }
    // Initial overlap: penetrating by 0.25 m along X reports the push-out direction and depth, and the displacement does not change it.
    {
        const OrientedBox box = saige::make_oriented_box({3.75f, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f);  // reaches x = 4.25 into a wall starting at 4
        for (const Vec3 d : {Vec3{0, 0, 0}, Vec3{1.0f, 0, 0}, Vec3{-1.0f, 0, 0}, Vec3{0, 2.0f, 0}}) {
            const auto hit = saige::sweep_oriented_box_aabb(box, d, wall);
            require(hit.has_value() && hit->started_overlapping, "initial overlap: reported whatever the displacement");
            require(hit->t == 0.0f && hit->contact_t == 0.0 && hit->stop == box.center, "initial overlap: stays put");
            require(near(hit->penetration, 0.25, 1e-5) && near(hit->normal.x, -1.0, 1e-6), "initial overlap: depth and the push-out direction");
        }
        const auto o = saige::oriented_box_aabb_overlap(box, wall);
        require(o.has_value() && near(o->depth, 0.25, 1e-5) && near(o->normal.x, -1.0, 1e-6), "overlap query: depth and direction");
        const OrientedBox clear = saige::make_oriented_box({3.0f, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f);
        require(!saige::oriented_box_aabb_overlap(clear, wall).has_value(), "no overlap when apart");
    }
    // Separated along an axis the box does not move along: it must never meet the wall, even though it moves toward the wall's far side.
    {
        const Aabb side_wall{{4.0f, 8.0f, -5.0f}, {5.0f, 10.0f, 5.0f}};
        const OrientedBox box = saige::make_oriented_box({0, 0, 0}, {1.0f, 0.5f, 0.5f}, 0.0f);
        require(!saige::sweep_oriented_box_aabb(box, {0, 12.0f, 0}, side_wall).has_value(), "separated on an axis it does not move along: no hit");
        require(!saige::sweep_oriented_box_aabb(saige::make_oriented_box({0, 0, 0}, {1.0f, 0.5f, 0.5f}, 0.7f), {0, 12.0f, 0}, side_wall).has_value(), "the same, turned: no hit");
    }
    // Two overlapped walls: the deeper initial overlap is the one reported, whatever their ids.
    {
        const std::vector<Aabb> walls{Aabb{{4.0f, -5.0f, -5.0f}, {5.0f, 5.0f, 5.0f}}, Aabb{{4.2f, -5.0f, -5.0f}, {5.0f, 5.0f, 5.0f}}};
        const OrientedBox box = saige::make_oriented_box({3.75f, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f);  // 0.25 into the first, 0.05 into the second
        const auto hit = saige::sweep_oriented_box(box, {1.0f, 0, 0}, walls, {});
        require(hit && hit->index == 0 && near(hit->penetration, 0.25, 1e-5), "the deeper initial overlap wins");
        const std::vector<Aabb> swapped{walls[1], walls[0]};
        const auto other = saige::sweep_oriented_box(box, {1.0f, 0, 0}, swapped, {});
        require(other && other->index == 1 && near(other->penetration, 0.25, 1e-5), "the deeper initial overlap wins whatever the order");
    }
    // The signed gap: apart by 1 m, touching, and overlapping by 0.25 m.
    {
        const OrientedBox box = saige::make_oriented_box({3.0f, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f);
        require(near(saige::oriented_box_aabb_gap(box, wall), 0.5, 1e-9), "gap: apart by 0.5 m");
        require(near(saige::oriented_box_aabb_gap(saige::make_oriented_box({3.5f, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f), wall), 0.0, 1e-9), "gap: touching");
        require(near(saige::oriented_box_aabb_gap(saige::make_oriented_box({3.75f, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f), wall), -0.25, 1e-6), "gap: overlapping by 0.25 m");
        const Triangle plane{{-5, -5, 0}, {5, -5, 0}, {0, 5, 0}};
        require(near(saige::oriented_box_triangle_gap(saige::make_oriented_box({0, 0, 2.0f}, {0.5f, 0.5f, 0.5f}, 0.3f), plane), 1.5, 1e-9), "gap: triangle");
    }
    // The exception to the clear stop: a box penetrating by less than the tolerance that moves into the surface is a t = 0 hit whose stop (the start) is
    // microscopically inside; stop_clear says so, and the penetration is reported. Exactly touching has a clear stop. A separated hit has one too.
    {
        const OrientedBox shallow = saige::make_oriented_box({3.5f + 5e-5f, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f);  // its face 0.05 mm into the wall at x = 4
        const auto into = saige::sweep_oriented_box_aabb(shallow, {1.0f, 0, 0}, wall);
        require(into.has_value() && !into->started_overlapping && into->contact_t == 0.0 && into->t == 0.0f, "sub-tolerance penetration moving in: a t = 0 hit, not an initial overlap");
        require(!into->stop_clear && near(into->penetration, 5e-5, 1e-6) && into->stop == shallow.center, "...whose stop is the start, flagged not clear, with the shallow depth");
        require(saige::oriented_box_aabb_gap(shallow, wall) < 0.0, "...consistent with the gap");
        const OrientedBox exact = saige::make_oriented_box({3.5f, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f);
        const auto flush = saige::sweep_oriented_box_aabb(exact, {1.0f, 0, 0}, wall);
        require(flush.has_value() && flush->stop_clear && flush->penetration == 0.0f, "exactly touching moving in: a clear stop");
        require(!saige::sweep_oriented_box_aabb(shallow, {-1.0f, 0, 0}, wall).has_value(), "sub-tolerance penetration moving away: no hit");
        const auto head_on = saige::sweep_oriented_box_aabb(saige::make_oriented_box({0, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f), {5.0f, 0, 0}, wall);
        require(head_on.has_value() && head_on->stop_clear && head_on->contact_t > 0.0, "a separated hit has a clear stop");
        const OrientedBox deep = saige::make_oriented_box({3.75f, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f);
        const auto overlapped = saige::sweep_oriented_box_aabb(deep, {1.0f, 0, 0}, wall);
        require(overlapped.has_value() && overlapped->started_overlapping && !overlapped->stop_clear, "an initial overlap's stop (the start) is not clear");
    }
    // Every function validates its primitives the same way as the sweep: an invalid box or triangle throws, in the pair, gap, stop and combined forms.
    {
        const OrientedBox box = saige::make_oriented_box({0, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        const Aabb flat{{1, 1, 1}, {1, 2, 2}}, bad_nan{{0, 0, 0}, {nan, 1, 1}};
        const Triangle degenerate{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}};
        const std::vector<Aabb> boxes_bad{flat};
        const std::vector<Triangle> tris_bad{degenerate};
        require(throws_invalid([&] { (void)saige::oriented_box_aabb_overlap(box, flat); }) && throws_invalid([&] { (void)saige::oriented_box_aabb_overlap(box, bad_nan); }), "overlap refuses an invalid box");
        require(throws_invalid([&] { (void)saige::oriented_box_aabb_gap(box, flat); }), "gap refuses an invalid box");
        require(throws_invalid([&] { (void)saige::oriented_box_triangle_overlap(box, degenerate); }) && throws_invalid([&] { (void)saige::oriented_box_triangle_gap(box, degenerate); }), "overlap and gap refuse a degenerate triangle");
        require(throws_invalid([&] { (void)saige::sweep_oriented_box_aabb(box, {1, 0, 0}, flat); }) && throws_invalid([&] { (void)saige::sweep_oriented_box_triangle(box, {1, 0, 0}, degenerate); }), "the pair sweeps refuse them");
        require(throws_invalid([&] { (void)saige::oriented_box_stop(box, {1, 0, 0}, 0.5, boxes_bad, {}); }) && throws_invalid([&] { (void)saige::oriented_box_stop(box, {1, 0, 0}, 0.5, {}, tris_bad); }),
                "oriented_box_stop refuses them");
        require(throws_invalid([&] { (void)saige::oriented_box_stop(box, {1, 0, 0}, nan, {}, {}); }) && throws_invalid([&] { (void)saige::oriented_box_stop(box, {100.0f, 0, 0}, 0.5, {}, {}); }),
                "oriented_box_stop refuses a non-finite fraction and a sweep outside the envelope");
        require(saige::oriented_box_stop(box, {1, 0, 0}, 0.0, {}, {}) == box.center, "oriented_box_stop returns the start for a zero fraction");
    }
    // Touching: a hit at t = 0 only when moving into the surface.
    {
        const OrientedBox touching = saige::make_oriented_box({3.5f, 0, 0}, {0.5f, 0.5f, 0.5f}, 0.0f);  // its face exactly on x = 4
        const auto into = saige::sweep_oriented_box_aabb(touching, {1.0f, 0, 0}, wall);
        require(into.has_value() && !into->started_overlapping && into->contact_t == 0.0 && into->t == 0.0f, "touching and moving in: a hit at t = 0");
        require(!saige::sweep_oriented_box_aabb(touching, {-1.0f, 0, 0}, wall).has_value(), "touching and moving away: no hit");
        require(!saige::sweep_oriented_box_aabb(touching, {0, 3.0f, 0}, wall).has_value(), "touching and sliding along the face: no hit");
        require(!saige::sweep_oriented_box_aabb(touching, {-1.0f, 2.0f, 0}, wall).has_value(), "touching and moving away at an angle: no hit");
        const OrientedBox floor_rest = saige::make_oriented_box({0, 0, 0.5f}, {0.5f, 0.5f, 0.5f}, 0.4f);
        const Aabb floor{{-50, -50, -1}, {50, 50, 0}};
        require(!saige::sweep_oriented_box_aabb(floor_rest, {8.0f, 3.0f, 0}, floor).has_value(), "resting on a floor and driving over it: no hit");
    }
}

void test_sweeps_match_an_independent_oracle() {
    Random random(0xB0C5ull);
    int checked_box = 0, checked_triangle = 0, hits = 0;
    for (int i = 0; i < 900; ++i) {
        const OrientedBox box = random_box(random, 4.0);
        const Vec3 d{random.range(-6, 6), random.range(-6, 6), random.range(-3, 3)};
        const Vec3 c{random.range(-5, 5), random.range(-5, 5), random.range(-2, 2)}, h{random.range(0.4, 2.5), random.range(0.4, 2.5), random.range(0.4, 2.5)};
        const Aabb wall{{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z + h.z}};
        const auto want = oracle_toi(box, d, [&](const Obb& o) { return solids_intersect(o, aabb_as_obb(wall)); });
        if (want && *want == 0.0) continue;  // an initial contact or overlap: covered by the dedicated cases
        const auto got = saige::sweep_oriented_box_aabb(box, d, wall);
        ++checked_box;
        require(got.has_value() == want.has_value(), "box " + std::to_string(i) + ": the sweep and the oracle disagree on whether there is a hit");
        if (got) {
            ++hits;
            require(near(got->contact_t, *want, 2e-6), "box " + std::to_string(i) + ": contact time " + std::to_string(got->contact_t) + " vs oracle " + std::to_string(*want));
            // The stop is clear and close to the contact; just past the contact the box overlaps.
            const OrientedBox at_stop = [&] { OrientedBox b = box; b.center = got->stop; return b; }();
            require(got->stop_clear, "box " + std::to_string(i) + ": a hit after t = 0 from a clear start reports a clear stop");
            require(!saige::oriented_box_aabb_overlap(at_stop, wall).has_value() && strictly_clear_of_aabb(box, d3(got->stop), wall), "box " + std::to_string(i) + ": the stop is clear");
            // The stop backs off along the motion until it is clear, so its distance from the contact is the gap it must close divided by the cosine of
            // the approach: measured along the contact normal it is a few microns at most.
            const double len = std::sqrt(double{d.x} * d.x + double{d.y} * d.y + double{d.z} * d.z);
            const double along_normal = std::fabs(double{d.x} * got->normal.x + double{d.y} * got->normal.y + double{d.z} * got->normal.z) / len;
            require(got->t <= got->contact_t + 1e-6 && (got->contact_t - got->t) * len * along_normal < 5e-5,
                    "box " + std::to_string(i) + ": the stop is within 50 microns of the contact along the normal (t " + std::to_string(got->t) + " contact " + std::to_string(got->contact_t) + " len " + std::to_string(len) + ")");
        }
    }
    for (int i = 0; i < 900; ++i) {
        const OrientedBox box = random_box(random, 4.0);
        const Vec3 d{random.range(-6, 6), random.range(-6, 6), random.range(-3, 3)};
        const auto o = [&] { return Vec3{random.range(-5, 5), random.range(-5, 5), random.range(-3, 3)}; };
        const Triangle t{o(), o(), o()};
        if (!saige::is_valid_triangle(t)) continue;
        const auto want = oracle_toi(box, d, [&](const Obb& ob) { return box_hits_triangle(ob, t); });
        if (want && *want == 0.0) continue;
        const auto got = saige::sweep_oriented_box_triangle(box, d, t);
        ++checked_triangle;
        require(got.has_value() == want.has_value(), "triangle " + std::to_string(i) + ": the sweep and the oracle disagree on whether there is a hit");
        if (got) {
            ++hits;
            require(near(got->contact_t, *want, 2e-6), "triangle " + std::to_string(i) + ": contact time " + std::to_string(got->contact_t) + " vs oracle " + std::to_string(*want));
            OrientedBox at_stop = box;
            at_stop.center = got->stop;
            require(!saige::oriented_box_triangle_overlap(at_stop, t).has_value() && strictly_clear_of_triangle(box, d3(got->stop), t), "triangle " + std::to_string(i) + ": the stop is clear");
        }
    }
    require(checked_box > 400 && checked_triangle > 400 && hits > 200, "the random sweeps exercise both hits and misses: " + std::to_string(checked_box) + " " + std::to_string(checked_triangle) + " " + std::to_string(hits));
}

void test_overlap_depth_against_oracle() {
    // Depth is the minimum translation: pushing the box out by it along the normal leaves it separated (touching), pushing less does not.
    Random random(0xD3917ull);
    int checked = 0;
    for (int i = 0; i < 400; ++i) {
        const OrientedBox box = random_box(random, 2.5);
        const Vec3 c{random.range(-2, 2), random.range(-2, 2), random.range(-1, 1)}, h{random.range(0.4, 2.0), random.range(0.4, 2.0), random.range(0.4, 2.0)};
        const Aabb wall{{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z + h.z}};
        const auto o = saige::oriented_box_aabb_overlap(box, wall);
        const bool really = solids_intersect(obb(box, d3(box.center)), aabb_as_obb(wall));
        require((saige::oriented_box_aabb_gap(box, wall) < 0.0) == really || std::fabs(saige::oriented_box_aabb_gap(box, wall)) < 1e-9, "the sign of the gap says whether the solids intersect");
        if (o) {
            require(really, "a reported overlap is a real intersection");
            const D3 n = {o->normal.x, o->normal.y, o->normal.z};
            const D3 pushed = add(d3(box.center), mul(n, double{o->depth} + 1e-4));
            require(!solids_intersect(obb(box, pushed), aabb_as_obb(wall)), "pushing out by depth separates the box");
            const D3 less = add(d3(box.center), mul(n, double{o->depth} * 0.9));
            require(solids_intersect(obb(box, less), aabb_as_obb(wall)), "pushing out by less does not");
            ++checked;
        }
    }
    require(checked > 60, "enough overlaps were checked: " + std::to_string(checked));
}

struct World {
    std::vector<Aabb> boxes;
    std::vector<Triangle> triangles;
};

World random_world(Random& random, int boxes, int triangles, double extent) {
    World w;
    for (int i = 0; i < boxes; ++i) {
        const Vec3 c{random.range(-extent, extent), random.range(-extent, extent), random.range(-extent * 0.2, extent * 0.2)};
        const Vec3 h{random.range(0.3, 3.0), random.range(0.3, 3.0), random.range(0.3, 3.0)};
        w.boxes.push_back({{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z + h.z}});
    }
    for (int i = 0; i < triangles; ++i) {
        const Vec3 o{random.range(-extent, extent), random.range(-extent, extent), random.range(-extent * 0.2, extent * 0.2)};
        const auto offset = [&] { return Vec3{o.x + random.range(-4, 4), o.y + random.range(-4, 4), o.z + random.range(-3, 3)}; };
        const Triangle t{o, offset(), offset()};
        if (saige::is_valid_triangle(t)) w.triangles.push_back(t);
    }
    return w;
}

void test_static_collision_equals_brute_force() {
    Random random(0x57A7Cull);
    int hits = 0, overlaps = 0, queries = 0;
    for (int world = 0; world < 5; ++world) {
        const World w = random_world(random, 60 + 40 * world, 120 + 60 * world, 60.0);
        const StaticCollision collision(w.boxes, w.triangles);
        saige::QueryScratch scratch;
        for (int i = 0; i < 160; ++i) {
            const OrientedBox box = random_box(random, 60.0);
            const Vec3 d{random.range(-8, 8), random.range(-8, 8), random.range(-3, 3)};
            const auto want = saige::sweep_oriented_box(box, d, w.boxes, w.triangles);
            const auto got = collision.sweep_oriented_box(box, d, scratch);
            ++queries;
            require(got.has_value() == want.has_value(), "static sweep: hit existence equals brute force");
            if (got) {
                ++hits;
                require(got->hit.contact_t == want->contact_t && got->hit.normal == want->normal && got->hit.penetration == want->penetration &&
                            got->hit.started_overlapping == want->started_overlapping,
                        "static sweep: contact, normal and overlap fields equal brute force");
                require(got->primitive.kind == (want->on_triangle ? StaticPrimitive::Kind::Triangle : StaticPrimitive::Kind::Box) && got->primitive.index == want->index && got->hit.index == want->index,
                        "static sweep: the same primitive");
                require(got->hit.t >= 0.0f && got->hit.t <= 1.0f, "static sweep: t in range");
                // The stop is clear of the whole neighborhood (the brute-force check is against everything).
                OrientedBox at_stop = box;
                at_stop.center = got->hit.stop;
                if (!got->hit.started_overlapping) {
                    for (const Aabb& b : w.boxes) require(strictly_clear_of_aabb(box, d3(got->hit.stop), b), "static sweep: the stop is clear of every box");
                    for (const Triangle& t : w.triangles) require(strictly_clear_of_triangle(box, d3(got->hit.stop), t), "static sweep: the stop is clear of every triangle");
                }
            }
            // Overlap queries: the deepest, and the full list in ascending order.
            std::optional<saige::StaticOrientedBoxOverlap> deepest;
            std::vector<saige::StaticOrientedBoxOverlap> all;
            for (std::size_t k = 0; k < w.boxes.size(); ++k) {
                if (const auto o = saige::oriented_box_aabb_overlap(box, w.boxes[k])) {
                    all.push_back({*o, {StaticPrimitive::Kind::Box, k}});
                    if (!deepest || o->depth > deepest->overlap.depth) deepest = all.back();
                }
            }
            for (std::size_t k = 0; k < w.triangles.size(); ++k) {
                if (const auto o = saige::oriented_box_triangle_overlap(box, w.triangles[k])) {
                    all.push_back({*o, {StaticPrimitive::Kind::Triangle, k}});
                    if (!deepest || o->depth > deepest->overlap.depth) deepest = all.back();
                }
            }
            const auto got_deepest = collision.overlap_oriented_box(box, scratch);
            require(got_deepest.has_value() == deepest.has_value(), "static overlap: existence");
            if (got_deepest) {
                ++overlaps;
                require(got_deepest->primitive == deepest->primitive && got_deepest->overlap.depth == deepest->overlap.depth && got_deepest->overlap.normal == deepest->overlap.normal, "static overlap: the deepest, lowest id on a tie");
            }
            std::vector<saige::StaticOrientedBoxOverlap> got_all;
            collision.overlaps_oriented_box(box, got_all, scratch);
            require(got_all.size() == all.size(), "static overlaps: the same count");
            for (std::size_t k = 0; k < all.size() && k < got_all.size(); ++k) {
                require(got_all[k].primitive == all[k].primitive && got_all[k].overlap.depth == all[k].overlap.depth, "static overlaps: ascending id order");
            }
        }
    }
    require(hits > 80 && overlaps > 20, "the equality checks exercise hits and overlaps: " + std::to_string(hits) + " " + std::to_string(overlaps));
    std::cout << "  " << queries << " oriented-box queries equal brute force (" << hits << " sweeps hit, " << overlaps << " overlaps)\n";
}

void test_ties_and_validation() {
    // Identical walls: the lower id wins an exact tie, a box wins an exact tie against a triangle.
    const Aabb wall{{4.0f, -5.0f, -5.0f}, {5.0f, 5.0f, 5.0f}};
    const std::vector<Aabb> two{wall, wall};
    const OrientedBox box = saige::make_oriented_box({0, 0, 0}, {1.0f, 0.5f, 0.5f}, 0.0f);
    const StaticCollision same(two, {});
    const auto tie = same.sweep_oriented_box(box, {5.0f, 0, 0});
    require(tie && tie->primitive.index == 0, "an exact tie goes to the lower id");
    const Triangle plane{{4, -5, -5}, {4, 5, -5}, {4, 0, 5}};  // the wall's near face
    const StaticCollision mixed(std::span<const Aabb>(&wall, 1), std::span<const Triangle>(&plane, 1));
    const auto mix = mixed.sweep_oriented_box(box, {5.0f, 0, 0});
    require(mix && mix->primitive.kind == StaticPrimitive::Kind::Box, "a box wins an exact tie against a triangle");

    // Validation.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    require(throws_invalid([&] { (void)saige::make_oriented_box({0, 0, 0}, {1.0f, 1.0f, 0.0f}, 0.0f); }), "a zero half extent is refused");
    require(throws_invalid([&] { (void)saige::make_oriented_box({0, 0, 0}, {1.0f, -1.0f, 1.0f}, 0.0f); }), "a negative half extent is refused");
    require(throws_invalid([&] { (void)saige::make_oriented_box({0, 0, 0}, {100.0f, 1.0f, 1.0f}, 0.0f); }), "an oversized half extent is refused");
    require(throws_invalid([&] { (void)saige::make_oriented_box({nan, 0, 0}, {1.0f, 1.0f, 1.0f}, 0.0f); }), "a NaN center is refused");
    require(throws_invalid([&] { (void)saige::make_oriented_box({0, 0, 0}, {1.0f, 1.0f, 1.0f}, nan); }), "a NaN yaw is refused");
    require(throws_invalid([&] { (void)saige::make_oriented_box({0, 0, 0}, {1.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f, 0.0f); }), "a zero quaternion is refused");
    OrientedBox skewed = box;
    skewed.axis_y = {0.6f, 0.8f, 0.0f};  // not perpendicular to axis_x
    require(throws_invalid([&] { saige::validate_oriented_box(skewed); }), "a skewed frame is refused");
    OrientedBox long_axis = box;
    long_axis.axis_x = {2.0f, 0.0f, 0.0f};
    require(throws_invalid([&] { saige::validate_oriented_box(long_axis); }), "a non-unit axis is refused");
    const StaticCollision empty;
    require(throws_invalid([&] { (void)empty.sweep_oriented_box(box, {100.0f, 0, 0}); }), "a sweep longer than max_sweep_length is refused, even with no geometry");
    require(throws_invalid([&] { (void)empty.sweep_oriented_box(saige::make_oriented_box({255.5f, 0, 0}, {1.0f, 1.0f, 1.0f}, 0.0f), {1.0f, 0, 0}); }), "leaving the +-256 m envelope is refused");
    require(throws_invalid([&] { (void)empty.sweep_oriented_box(box, {nan, 0, 0}); }), "a NaN displacement is refused");
    require(throws_invalid([&] { (void)empty.overlap_oriented_box(saige::make_oriented_box({300.0f, 0, 0}, {1.0f, 1.0f, 1.0f}, 0.0f)); }), "an overlap query outside the envelope is refused");
    require(!empty.sweep_oriented_box(box, {5.0f, 0, 0}).has_value() && !empty.overlap_oriented_box(box).has_value(), "empty geometry: nothing");

    // The two constructors agree: a yaw about Z is the quaternion about Z.
    const float yaw = 0.9f;
    const OrientedBox a = saige::make_oriented_box({1, 2, 3}, {1, 1, 1}, yaw);
    const OrientedBox b = saige::make_oriented_box({1, 2, 3}, {1, 1, 1}, 0.0f, 0.0f, std::sin(yaw / 2), std::cos(yaw / 2));
    require(near(a.axis_x.x, b.axis_x.x, 1e-6) && near(a.axis_x.y, b.axis_x.y, 1e-6) && near(a.axis_y.x, b.axis_y.x, 1e-6) && near(a.axis_y.y, b.axis_y.y, 1e-6), "yaw and quaternion agree");
    require(near(a.axis_x.x, std::cos(yaw), 1e-6) && near(a.axis_x.y, std::sin(yaw), 1e-6), "local +X turns counter-clockwise from above");

    // Determinism: the same query twice gives the same bytes.
    Random random(7);
    const World w = random_world(random, 40, 80, 40.0);
    const StaticCollision collision(w.boxes, w.triangles);
    const OrientedBox q = random_box(random, 30.0);
    const auto r1 = collision.sweep_oriented_box(q, {5.0f, 3.0f, -1.0f}), r2 = collision.sweep_oriented_box(q, {5.0f, 3.0f, -1.0f});
    require(r1.has_value() == r2.has_value() && (!r1 || (r1->hit.contact_t == r2->hit.contact_t && r1->hit.stop == r2->hit.stop && r1->primitive == r2->primitive)), "deterministic");
}

} // namespace

int main() {
    try {
        test_analytic_cases();
        test_sweeps_match_an_independent_oracle();
        test_overlap_depth_against_oracle();
        test_static_collision_equals_brute_force();
        test_ties_and_validation();
    } catch (const std::exception& error) {
        std::cerr << "oriented box test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "oriented box tests passed\n";
}
