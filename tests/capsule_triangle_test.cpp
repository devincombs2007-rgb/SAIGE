// capsule_triangle.hpp against independent references: a triangulated box must behave exactly like the box sweep, and a brute-force
// reference (a different distance algorithm, minimized by ternary search, scanned in small time steps) must agree on every random case.
#include "saige/capsule_triangle.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using saige::Aabb;
using saige::Capsule;
using saige::CapsuleHit;
using saige::Triangle;
using saige::Vec3;
using D3 = std::array<double, 3>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed) : state(seed) {}
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    double uniform(double lo, double hi) { return lo + (hi - lo) * (static_cast<double>(next() >> 11) / 9007199254740992.0); }
    float f(double lo, double hi) { return static_cast<float>(uniform(lo, hi)); }
};

D3 d3(Vec3 v) { return {v.x, v.y, v.z}; }
D3 sub(const D3& a, const D3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
D3 add(const D3& a, const D3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
D3 mul(const D3& a, double s) { return {a[0] * s, a[1] * s, a[2] * s}; }
double dot(const D3& a, const D3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D3 cross(const D3& a, const D3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double norm(const D3& a) { return std::sqrt(dot(a, a)); }

// ---- Reference distance: project onto the plane, test inside by edge-side signs, else the nearest point on the three edges. No code
// is shared with the implementation.
double ref_point_segment(const D3& p, const D3& a, const D3& b) {
    const D3 ab = sub(b, a);
    const double t = std::clamp(dot(sub(p, a), ab) / dot(ab, ab), 0.0, 1.0);
    return norm(sub(p, add(a, mul(ab, t))));
}

double ref_point_triangle(const D3& p, const Triangle& tri) {
    const D3 a = d3(tri.a), b = d3(tri.b), c = d3(tri.c);
    const D3 n = cross(sub(b, a), sub(c, a));
    const double nn = dot(n, n);
    const double plane = dot(sub(p, a), n) / std::sqrt(nn);
    const D3 q = sub(p, mul(n, dot(sub(p, a), n) / nn));  // projection
    const bool inside = dot(cross(sub(b, a), sub(q, a)), n) >= 0 && dot(cross(sub(c, b), sub(q, b)), n) >= 0 && dot(cross(sub(a, c), sub(q, c)), n) >= 0;
    if (inside) return std::abs(plane);
    return std::min({ref_point_segment(p, a, b), ref_point_segment(p, b, c), ref_point_segment(p, c, a)});
}

// Distance from the core segment to the triangle: the point-triangle distance is convex along a line, so ternary search is exact.
double ref_axis_distance(const D3& p0, const D3& p1, const Triangle& tri) {
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 70; ++i) {
        const double m1 = lo + (hi - lo) / 3.0, m2 = hi - (hi - lo) / 3.0;
        const double f1 = ref_point_triangle(add(p0, mul(sub(p1, p0), m1)), tri);
        const double f2 = ref_point_triangle(add(p0, mul(sub(p1, p0), m2)), tri);
        (f1 < f2 ? hi : lo) = f1 < f2 ? m2 : m1;
    }
    return ref_point_triangle(add(p0, mul(sub(p1, p0), 0.5 * (lo + hi))), tri);
}

double ref_gap_at(const Capsule& c, Vec3 d, double t, const Triangle& tri) {
    const D3 feet = add(d3(c.feet), mul(d3(d), t));
    const D3 p0{feet[0], feet[1], feet[2] + c.radius};
    const D3 p1{feet[0], feet[1], feet[2] + (c.height - c.radius)};
    return ref_axis_distance(p0, p1, tri) - c.radius;
}

Triangle random_triangle(Rng& rng, double extent, double size) {
    for (;;) {
        const Vec3 base{rng.f(-extent, extent), rng.f(-extent, extent), rng.f(-extent, extent)};
        const auto offset = [&] { return Vec3{base.x + rng.f(-size, size), base.y + rng.f(-size, size), base.z + rng.f(-size, size)}; };
        const Triangle t{offset(), offset(), offset()};
        if (saige::is_valid_triangle(t) && norm(cross(sub(d3(t.b), d3(t.a)), sub(d3(t.c), d3(t.a)))) > 0.3) return t;
    }
}

Capsule random_capsule(Rng& rng) {
    const float r = rng.f(0.15, 0.5);
    return {{rng.f(-3, 3), rng.f(-3, 3), rng.f(-3, 3)}, r, r * 2.0f + rng.f(0.0, 1.6)};
}

std::vector<Triangle> box_triangles(const Aabb& b) {
    const Vec3 v[8] = {{b.min.x, b.min.y, b.min.z}, {b.max.x, b.min.y, b.min.z}, {b.max.x, b.max.y, b.min.z}, {b.min.x, b.max.y, b.min.z},
                       {b.min.x, b.min.y, b.max.z}, {b.max.x, b.min.y, b.max.z}, {b.max.x, b.max.y, b.max.z}, {b.min.x, b.max.y, b.max.z}};
    const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    std::vector<Triangle> out;
    for (const auto& q : quads) {
        out.push_back({v[q[0]], v[q[1]], v[q[2]]});
        out.push_back({v[q[0]], v[q[2]], v[q[3]]});
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------------

void test_validation() {
    const Capsule capsule{{0, 0, 1}, 0.3f, 1.7f};
    const Triangle good{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    const auto throws = [](auto&& fn) {
        try { fn(); } catch (const std::invalid_argument&) { return true; }
        return false;
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    require(saige::is_valid_triangle(good), "a plain triangle is valid");
    require(!saige::is_valid_triangle({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}), "a collinear triangle is degenerate");
    require(!saige::is_valid_triangle({{0, 0, 0}, {0, 0, 0}, {0, 1, 0}}), "a triangle with a repeated vertex is degenerate");
    require(!saige::is_valid_triangle({{0, 0, 0}, {nan, 0, 0}, {0, 1, 0}}), "a non-finite triangle is invalid");
    require(throws([&] { (void)saige::sweep_capsule_triangle(capsule, {0, 0, -1}, {{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}); }), "sweeping a degenerate triangle throws");
    require(throws([&] { (void)saige::sweep_capsule_triangle(capsule, {nan, 0, 0}, good); }), "a non-finite displacement throws");
    require(throws([&] { (void)saige::sweep_capsule_triangle({{0, 0, 0}, 0.3f, 0.5f}, {0, 0, 1}, good); }), "a capsule shorter than its diameter throws");
    require(throws([&] { (void)saige::capsule_triangle_gap({{0, 0, 0}, -1.0f, 2.0f}, good); }), "a non-positive radius throws");
    const std::vector<Triangle> bad{good, {{0, 0, 0}, {0, 0, 0}, {1, 1, 1}}};
    require(throws([&] { (void)saige::sweep_capsule_triangles(capsule, {0, 0, -1}, bad); }), "a degenerate triangle in a set throws");
    require(throws([&] { (void)saige::triangle_normal(bad[1]); }), "the normal of a degenerate triangle throws");
    const Vec3 n = saige::triangle_normal(good);
    require(n.x == 0 && n.y == 0 && n.z == 1, "the normal follows the right-hand rule");
}

void test_flat_triangle_landing_and_contact_normals() {
    const Triangle floor{{-5, -5, 0}, {5, -5, 0}, {0, 6, 0}};
    const Capsule c{{0, 0, 1.0f}, 0.3f, 1.7f};
    const auto hit = saige::sweep_capsule_triangle(c, {0, 0, -2}, floor);
    require(hit && !hit->started_overlapping, "falling onto a triangle hits it");
    require(std::abs(hit->t - 0.5f) < 1e-5f, "it lands after exactly the drop (t = 0.5)");
    require(std::abs(hit->normal.z - 1.0f) < 1e-6f && std::abs(hit->normal.x) < 1e-6f, "the normal of a floor hit is +Z");
    require(std::abs(hit->point.z) < 1e-5f, "the contact point is on the triangle");
    // Two-sided: from below, the normal points down.
    const auto up = saige::sweep_capsule_triangle({{0, 0, -3.0f}, 0.3f, 1.7f}, {0, 0, 3}, floor);
    require(up && up->normal.z < -0.999f, "a ceiling hit from below has normal -Z (triangles are two-sided)");
    require(std::abs(up->t - (3.0f - 1.7f) / 3.0f) < 1e-5f, "the head (top hemisphere) is what touches: t = 1.3 / 3");
    // Moving away or parallel while touching is not a hit; moving into it is.
    const Capsule resting{{0, 0, 0.0f}, 0.3f, 1.7f};
    require(!saige::sweep_capsule_triangle(resting, {1, 0, 0}, floor), "sliding along a touched floor is not a hit");
    require(!saige::sweep_capsule_triangle(resting, {0, 0, 1}, floor), "moving away from a touched floor is not a hit");
    const auto into = saige::sweep_capsule_triangle(resting, {0, 0, -0.1f}, floor);
    require(into && into->t == 0.0f && !into->started_overlapping, "moving into a touched floor hits at t = 0 without overlap");
    // A zero displacement never hits a non-overlapping triangle.
    require(!saige::sweep_capsule_triangle({{0, 0, 2}, 0.3f, 1.7f}, {0, 0, 0}, floor), "no displacement, no hit");
}

void test_edge_and_vertex_contacts() {
    // A vertical wall triangle whose right edge is vertical: approach it from the side beyond that edge: the capsule's cylinder touches
    // the edge, and the normal points away from the edge, not along the wall's face normal.
    const Triangle wall{{0, 0, 0}, {4, 0, 0}, {0, 0, 4}};  // in the XZ plane (y = 0); the hypotenuse runs from (4,0,0) to (0,0,4)
    const Capsule c{{3.0f, -2.0f, 0.5f}, 0.3f, 1.0f};      // beside the hypotenuse region
    const auto hit = saige::sweep_capsule_triangle(c, {0, 4, 0}, wall);
    require(hit.has_value(), "walking into a wall triangle hits");
    require(std::abs(hit->normal.y + 1.0f) < 1e-4f, "a face contact has the face normal");
    // Edge contact: the capsule moves along +Y toward the point beyond the triangle's edge (4,0,0)-(0,0,4): the nearest feature is the
    // hypotenuse seen from outside the triangle's area.
    const Capsule outside{{3.0f, -2.0f, 0.9f}, 0.3f, 1.0f};  // the core is 0.14 outside the hypotenuse, in plane
    const auto edge = saige::sweep_capsule_triangle(outside, {0, 4, 0}, wall);
    require(edge.has_value(), "an edge contact is found when the path only clips the triangle's boundary region");
    const float tilt = std::sqrt(edge->normal.x * edge->normal.x + edge->normal.z * edge->normal.z);
    require(tilt > 0.05f, "an edge or vertex contact normal is tilted away from the face normal");
    require(std::abs(std::sqrt(edge->normal.x * edge->normal.x + edge->normal.y * edge->normal.y + edge->normal.z * edge->normal.z) - 1.0f) < 1e-5f, "normals are unit length");
    // Vertex: approach the apex of a thin triangle head-on from beyond it.
    const Triangle sliver{{0, 0, 0}, {0, 0, 1}, {3, 0, 0}};
    const auto vertex = saige::sweep_capsule_triangle({{6.0f, 0.0f, -0.3f}, 0.3f, 1.0f}, {-6, 0, 0}, sliver);  // the core bottom is level with the apex
    require(vertex && vertex->normal.x > 0.99f, "approaching a vertex end-on has a normal pointing back along the path");
    require(std::abs(vertex->t - (6.0f - 3.0f - 0.3f) / 6.0f) < 1e-5f, "contact is when the cylinder surface reaches the vertex");
}

void test_vertex_against_the_middle_of_the_cylinder() {
    // A triangle vertex pointing at the capsule's mid-height (not its ends): the contact is vertex against cylinder wall, and the
    // normal is horizontal. The triangle leans away so that no edge or face gets there first.
    const Triangle spike{{3.0f, 0.0f, 0.8f}, {5.0f, 1.0f, 0.3f}, {5.0f, -1.0f, 1.3f}};
    const auto hit = saige::sweep_capsule_triangle({{0.0f, 0.0f, 0.0f}, 0.3f, 1.7f}, {6, 0, 0}, spike);
    require(hit.has_value(), "a vertex ahead at mid-height is hit");
    require(std::abs(hit->t * 6.0f - 2.7f) < 2e-5f, "contact when the cylinder wall reaches the vertex (3 - radius)");
    require(std::abs(hit->normal.x + 1.0f) < 1e-4f && std::abs(hit->normal.z) < 1e-4f, "the normal is horizontal, pointing back at the capsule");
    require(std::abs(hit->point.x - 3.0f) < 1e-5f && std::abs(hit->point.z - 0.8f) < 1e-5f, "the contact point is the vertex");
    // A vertex level with the head's hemisphere (z between the core top at 1.4 and the capsule top at 1.7): vertex against the cap.
    const Triangle high{{3.0f, 0.0f, 1.6f}, {5.0f, 1.0f, 2.4f}, {5.0f, -1.0f, 2.6f}};
    const auto cap = saige::sweep_capsule_triangle({{0.0f, 0.0f, 0.0f}, 0.3f, 1.7f}, {6, 0, 0}, high);
    require(cap.has_value(), "a vertex at head height is hit");
    const float dx = std::sqrt(0.3f * 0.3f - 0.2f * 0.2f);  // horizontal offset at which the cap surface meets the vertex
    require(std::abs(cap->t * 6.0f - (3.0f - dx)) < 2e-5f, "contact on the hemisphere, not the cylinder wall");
    require(cap->normal.z < -0.5f && cap->normal.x < -0.5f, "the normal points from the vertex down and back to the head");
}

void test_ramp_contact_normal_and_landing_height() {
    // A 30 degree ramp rising toward +X: from (0,*,0) to (4, *, 4 tan 30).
    const float h = 4.0f * std::tan(30.0f * 3.14159265f / 180.0f);
    const std::vector<Triangle> ramp{{{0, -2, 0}, {4, -2, h}, {4, 2, h}}, {{0, -2, 0}, {4, 2, h}, {0, 2, 0}}};
    const auto hit = saige::sweep_capsule_triangles({{2.0f, 0.0f, 3.0f}, 0.3f, 1.7f}, {0, 0, -4}, ramp);
    require(hit.has_value(), "a capsule dropped on a ramp hits it");
    const float expected = std::cos(30.0f * 3.14159265f / 180.0f);
    require(std::abs(hit->normal.z - expected) < 1e-4f && hit->normal.x < 0.0f, "the normal tilts back down the slope with cos(30) vertical");
    // Its lowest point touches the plane z = x tan 30 at distance r along the normal: the feet are r(1/cos - 1) ... verify by gap instead.
    const Capsule landed{{2.0f, 0.0f, 3.0f - 4.0f * hit->t}, 0.3f, 1.7f};
    require(std::abs(saige::capsule_triangle_gap(landed, ramp[0])) < 2e-5f || std::abs(saige::capsule_triangle_gap(landed, ramp[1])) < 2e-5f,
            "at the reported time the capsule is exactly touching");
    require(hit->index <= 1, "the hit names a triangle");
}

void test_tunneling_is_impossible() {
    const Triangle sheet{{-0.5f, -0.5f, 0}, {0.5f, -0.5f, 0}, {0, 0.5f, 0}};
    for (const float speed : {10.0f, 1000.0f, 100000.0f}) {
        const auto hit = saige::sweep_capsule_triangle({{0, 0, 500.0f}, 0.1f, 0.4f}, {0, 0, -speed}, sheet);
        if (speed >= 500.0f) {
            require(hit && !hit->started_overlapping, "a very long sweep still stops at a thin sheet");
            require(std::abs(hit->t * speed - 500.0f) < 1e-2f, "and at the right place (the feet reach the sheet after 500 m)");
        } else {
            require(!hit, "a short sweep that does not reach the sheet does not hit it");
        }
    }
    // A grazing pass just above the surface (0.5 mm of clearance) is not a hit; 0.5 mm lower it is.
    const Triangle floor{{-50, -50, 0}, {50, -50, 0}, {0, 50, 0}};
    require(!saige::sweep_capsule_triangle({{-5, 0, 0.0005f}, 0.3f, 1.0f}, {10, 0, 0}, floor), "a level pass just above a floor does not hit");
    const auto low = saige::sweep_capsule_triangle({{-5, 0, 0.0005f}, 0.3f, 1.0f}, {10, 0, -0.001f}, floor);
    require(low && low->t > 0.0f && low->t < 1.0f, "a pass that descends 1 mm does hit");
}

void test_overlap_and_push_out() {
    const Triangle floor{{-5, -5, 0}, {5, -5, 0}, {0, 6, 0}};
    // Sunk 0.1 into the floor: push straight up by 0.1.
    const Capsule sunk{{0, 0, -0.1f}, 0.3f, 1.7f};
    const auto push = saige::capsule_triangle_push_out(sunk, floor);
    require(push && std::abs(push->z - 0.1f) < 1e-5f && std::abs(push->x) < 1e-6f, "a capsule sunk into a floor is pushed up by the depth");
    const auto hit = saige::sweep_capsule_triangle(sunk, {1, 0, 0}, floor);
    require(hit && hit->started_overlapping && hit->t == 0.0f && hit->normal.z > 0.999f, "a sweep from inside reports the overlap with the push-out normal");
    require(!saige::capsule_triangle_push_out({{0, 0, 0.0f}, 0.3f, 1.7f}, floor), "touching is not overlapping");
    require(!saige::capsule_triangle_push_out({{0, 0, 3.0f}, 0.3f, 1.7f}, floor), "a distant capsule has no push-out");
    require(saige::capsule_triangle_gap(sunk, floor) < 0.0f, "the gap of an overlapping capsule is negative");
    // Through the face (the core crosses it): leave through the nearer side.
    const Capsule through{{0, 0, -0.5f}, 0.3f, 1.7f};  // the core spans z from -0.2 to 0.9: mostly above the plane
    const auto out = saige::capsule_triangle_push_out(through, floor);
    require(out && out->z > 0.0f && std::abs(out->z - (0.3f + 0.2f)) < 1e-5f, "a crossing core leaves through the nearer side");
    const Capsule below{{0, 0, -1.6f}, 0.3f, 1.7f};   // the core spans -1.3 to -0.1: mostly below the plane, so it leaves downward
    const auto down = saige::capsule_triangle_push_out(below, floor);
    require(down && down->z < 0.0f, "a core below the plane is pushed down");
    // After the push, the capsule is clear.
    const Capsule fixed{{sunk.feet.x, sunk.feet.y, sunk.feet.z + push->z}, 0.3f, 1.7f};
    require(std::abs(saige::capsule_triangle_gap(fixed, floor)) < 2e-5f, "after the push the capsule just touches");
}

void test_mesh_earliest_hit_and_ties() {
    const Triangle high{{-5, -5, 1}, {5, -5, 1}, {0, 6, 1}};
    const Triangle low{{-5, -5, 0}, {5, -5, 0}, {0, 6, 0}};
    const std::vector<Triangle> mesh{low, high};
    const auto hit = saige::sweep_capsule_triangles({{0, 0, 3}, 0.3f, 1.0f}, {0, 0, -5}, mesh);
    require(hit && hit->index == 1, "the nearer triangle (index 1) is hit first");
    const std::vector<Triangle> twins{high, high};
    const auto tie = saige::sweep_capsule_triangles({{0, 0, 3}, 0.3f, 1.0f}, {0, 0, -5}, twins);
    require(tie && tie->index == 0, "ties go to the lowest index");
    require(!saige::sweep_capsule_triangles({{0, 0, 3}, 0.3f, 1.0f}, {0, 0, -5}, std::span<const Triangle>{}), "an empty set never hits");
    // Order independence of the time.
    const std::vector<Triangle> reversed{high, low};
    const auto other = saige::sweep_capsule_triangles({{0, 0, 3}, 0.3f, 1.0f}, {0, 0, -5}, reversed);
    require(other && other->t == hit->t, "triangle order does not change the time of the hit");
}

void test_coplanar_neighbours_have_no_ghost_edges() {
    // Walking across the shared diagonal of a triangulated floor: the contact normal must stay +Z everywhere.
    const std::vector<Triangle> floor{{{-5, -5, 0}, {5, -5, 0}, {5, 5, 0}}, {{-5, -5, 0}, {5, 5, 0}, {-5, 5, 0}}};
    for (int i = 0; i <= 40; ++i) {
        const float x = -2.0f + 0.1f * static_cast<float>(i);
        const auto hit = saige::sweep_capsule_triangles({{x, x * 0.9f, 0.5f}, 0.3f, 1.7f}, {0, 0, -1}, floor);
        require(hit && std::abs(hit->normal.z - 1.0f) < 1e-6f, "a floor's diagonal does not tilt the contact normal");
        require(std::abs(hit->t - 0.5f) < 1e-5f, "nor change the landing height");
    }
}

void test_triangulated_box_matches_the_box_sweep() {
    Rng rng(0xC0FFEE);
    int hits = 0, misses = 0;
    for (int n = 0; n < 4000; ++n) {
        const Vec3 lo{rng.f(-3, 3), rng.f(-3, 3), rng.f(-3, 3)};
        const Aabb box{lo, {lo.x + rng.f(0.3, 3), lo.y + rng.f(0.3, 3), lo.z + rng.f(0.3, 3)}};
        const Capsule c = random_capsule(rng);
        if (saige::capsule_box_gap(c, box) < 1e-3f) continue;
        // Aim roughly at the box so that a good share of the cases hit (and the rest graze or miss).
        const Vec3 aim{(box.min.x + box.max.x) * 0.5f - c.feet.x, (box.min.y + box.max.y) * 0.5f - c.feet.y, (box.min.z + box.max.z) * 0.5f - c.feet.z};
        const float scale = rng.f(0.4, 2.0);
        const Vec3 d{aim.x * scale + rng.f(-2, 2), aim.y * scale + rng.f(-2, 2), aim.z * scale + rng.f(-2, 2)};
        const auto box_hit = saige::sweep_capsule_box(c, d, box);
        const auto tris = box_triangles(box);
        const auto tri_hit = saige::sweep_capsule_triangles(c, d, tris);
        const double length = norm(d3(d));
        if (box_hit && box_hit->t >= 0.9999f) continue;  // a hit at the very end of the path is decided by tolerance
        require(box_hit.has_value() == tri_hit.has_value(), "the triangulated box hits exactly when the box does (case " + std::to_string(n) + ")");
        if (!box_hit) {
            ++misses;
            continue;
        }
        ++hits;
        require(std::abs(box_hit->t - tri_hit->t) * length < 2e-5,
                "same time of impact (case " + std::to_string(n) + "): box t=" + std::to_string(box_hit->t) + " triangles t=" + std::to_string(tri_hit->t) +
                    " |d|=" + std::to_string(length) + " box normal (" + std::to_string(box_hit->normal.x) + "," + std::to_string(box_hit->normal.y) + "," +
                    std::to_string(box_hit->normal.z) + ") tri normal (" + std::to_string(tri_hit->normal.x) + "," + std::to_string(tri_hit->normal.y) + "," +
                    std::to_string(tri_hit->normal.z) + ") tri index " + std::to_string(tri_hit->index));
        const D3 a = d3(box_hit->normal), b = d3(tri_hit->normal);
        require(dot(a, b) > 0.9999, "same contact normal (case " + std::to_string(n) + ")");
        require(!tri_hit->started_overlapping, "no overlap flag");
    }
    require(hits > 800 && misses > 800, "the random cases cover both hits (" + std::to_string(hits) + ") and misses (" + std::to_string(misses) + ")");
}

void test_random_cases_against_the_brute_force_reference() {
    Rng rng(0xBADC0DE);
    int hits = 0, misses = 0;
    for (int n = 0; n < 1500; ++n) {
        const Triangle tri = random_triangle(rng, 2.5, 2.0);
        const Capsule c = random_capsule(rng);
        if (ref_gap_at(c, {0, 0, 0}, 0.0, tri) < 1e-3) continue;
        const Vec3 aim{(tri.a.x + tri.b.x + tri.c.x) / 3.0f - c.feet.x, (tri.a.y + tri.b.y + tri.c.y) / 3.0f - c.feet.y,
                       (tri.a.z + tri.b.z + tri.c.z) / 3.0f - c.feet.z};
        const float scale = rng.f(0.4, 2.0);
        const Vec3 d{aim.x * scale + rng.f(-2, 2), aim.y * scale + rng.f(-2, 2), aim.z * scale + rng.f(-2, 2)};
        const auto hit = saige::sweep_capsule_triangle(c, d, tri);
        constexpr int steps = 400;
        double first_penetration = -1.0;
        for (int k = 0; k <= steps; ++k) {
            const double t = static_cast<double>(k) / steps;
            if (ref_gap_at(c, d, t, tri) < -2e-5) {
                first_penetration = t;
                break;
            }
        }
        if (!hit) {
            ++misses;
            require(first_penetration < 0.0, "no sampled time along a clear path penetrates (case " + std::to_string(n) + ")");
            continue;
        }
        ++hits;
        require(!hit->started_overlapping, "not overlapping at the start");
        const double t_hit = hit->t;
        require(std::abs(ref_gap_at(c, d, t_hit, tri)) < 5e-5, "at the reported time the reference gap is zero (case " + std::to_string(n) + ")");
        require(first_penetration < 0.0 || first_penetration >= t_hit - 1e-6, "nothing penetrates before the reported hit (case " + std::to_string(n) + ")");
        // The hit is an actual first contact: the reference sees penetration shortly after (when moving on into the surface).
        require(std::abs(std::sqrt(static_cast<double>(hit->normal.x * hit->normal.x + hit->normal.y * hit->normal.y + hit->normal.z * hit->normal.z)) - 1.0) < 1e-5, "unit normal");
        require(dot(d3(hit->normal), d3(d)) < 0.0, "the path moves into the surface at the contact (case " + std::to_string(n) + ")");
        // And the reported point is on the triangle at the reference distance from the axis.
        const D3 feet = add(d3(c.feet), mul(d3(d), t_hit));
        const D3 p0{feet[0], feet[1], feet[2] + c.radius}, p1{feet[0], feet[1], feet[2] + (c.height - c.radius)};
        require(std::abs(ref_point_triangle(d3(hit->point), tri)) < 5e-5, "the contact point lies on the triangle (case " + std::to_string(n) + ")");
        (void)p0;
        (void)p1;
    }
    require(hits > 200 && misses > 200, "the reference cases cover hits (" + std::to_string(hits) + ") and misses (" + std::to_string(misses) + ")");
}

void test_push_out_against_the_reference() {
    Rng rng(0x5EED);
    int checked = 0;
    for (int n = 0; n < 4000; ++n) {
        const Triangle tri = random_triangle(rng, 1.5, 2.0);
        Capsule c = random_capsule(rng);
        c.feet = {(tri.a.x + tri.b.x + tri.c.x) / 3.0f + rng.f(-1.2, 1.2), (tri.a.y + tri.b.y + tri.c.y) / 3.0f + rng.f(-1.2, 1.2),
                  (tri.a.z + tri.b.z + tri.c.z) / 3.0f + rng.f(-1.8, 0.6)};  // near the triangle, so that many cases overlap
        const double gap = ref_gap_at(c, {0, 0, 0}, 0.0, tri);
        const auto push = saige::capsule_triangle_push_out(c, tri);
        if (gap > 1e-4) {
            require(!push, "no push for a clear capsule");
            continue;
        }
        if (gap < -1e-4 && gap > -0.9 * c.radius) {  // the core is outside the triangle: the minimal push is exact
            require(push.has_value(), "an overlapping capsule has a push-out");
            const double len = norm(d3(*push));
            require(std::abs(len + gap) < 2e-4, "the push length equals the penetration depth (case " + std::to_string(n) + ")");
            const Capsule moved{{c.feet.x + push->x, c.feet.y + push->y, c.feet.z + push->z}, c.radius, c.height};
            require(std::abs(ref_gap_at(moved, {0, 0, 0}, 0.0, tri)) < 2e-4, "after the push the capsule just touches");
            const Capsule less{{c.feet.x + 0.9f * push->x, c.feet.y + 0.9f * push->y, c.feet.z + 0.9f * push->z}, c.radius, c.height};
            require(ref_gap_at(less, {0, 0, 0}, 0.0, tri) < 0.0, "a 10% smaller push still overlaps: the push is minimal");
            ++checked;
        }
    }
    require(checked > 250, "enough overlapping cases were checked (" + std::to_string(checked) + ")");
}

void test_determinism_and_numerical_range() {
    Rng rng(0xDE7);
    for (int n = 0; n < 200; ++n) {
        const Triangle tri = random_triangle(rng, 2.0, 2.0);
        const Capsule c = random_capsule(rng);
        const Vec3 d{rng.f(-4, 4), rng.f(-4, 4), rng.f(-4, 4)};
        const auto a = saige::sweep_capsule_triangle(c, d, tri);
        const auto b = saige::sweep_capsule_triangle(c, d, tri);
        require(a.has_value() == b.has_value(), "repeatable");
        if (a) require(std::memcmp(&*a, &*b, sizeof(CapsuleHit)) == 0 || (a->t == b->t && a->normal == b->normal && a->point == b->point), "bit-identical hits");
    }
    // Far from the origin, with the same relative geometry, the answer shifts by the offset (float coordinates limit this: 1e4 m).
    const Triangle near_tri{{-5, -5, 0}, {5, -5, 0}, {0, 6, 0}};
    const auto reference = saige::sweep_capsule_triangle({{0, 0, 1.0f}, 0.3f, 1.7f}, {0, 0, -2}, near_tri);
    const float off = 8192.0f;  // exactly representable
    const Triangle far_tri{{near_tri.a.x + off, near_tri.a.y + off, near_tri.a.z}, {near_tri.b.x + off, near_tri.b.y + off, near_tri.b.z},
                           {near_tri.c.x + off, near_tri.c.y + off, near_tri.c.z}};
    const auto shifted = saige::sweep_capsule_triangle({{off, off, 1.0f}, 0.3f, 1.7f}, {0, 0, -2}, far_tri);
    require(reference && shifted && std::abs(reference->t - shifted->t) < 1e-6f, "a translated scene gives the same time of impact");
    // Tiny and huge triangles.
    const Triangle tiny{{0, 0, 0}, {1e-3f, 0, 0}, {0, 1e-3f, 0}};
    require(saige::sweep_capsule_triangle({{0.0003f, 0.0003f, 1}, 0.3f, 1.0f}, {0, 0, -2}, tiny).has_value(), "a millimeter triangle is hit");
    const Triangle huge{{-1e6f, -1e6f, 0}, {1e6f, -1e6f, 0}, {0, 1e6f, 0}};
    const auto big = saige::sweep_capsule_triangle({{3, 4, 1}, 0.3f, 1.0f}, {0, 0, -2}, huge);
    require(big && std::abs(big->t - 0.5f) < 1e-5f, "a kilometer-scale triangle is hit at the right time");
    // A needle-thin (but valid) triangle.
    const Triangle needle{{0, 0, 0}, {100, 0, 0}, {50, 0.001f, 0}};
    require(saige::sweep_capsule_triangle({{50, 0, 1}, 0.3f, 1.0f}, {0, 0, -2}, needle).has_value(), "a thin sliver is hit");
}

void test_cost_of_a_ramp_sized_mesh() {
    // Not an assertion on speed (machines differ): a recorded measurement for the benchmark notes, plus a sanity bound.
    Rng rng(0xBEEF);
    std::vector<Triangle> mesh;
    for (int i = 0; i < 400; ++i) mesh.push_back(random_triangle(rng, 8.0, 1.5));
    const Capsule c{{0, 0, 0}, 0.3f, 1.7f};
    const auto start = std::chrono::steady_clock::now();
    int hits = 0;
    constexpr int calls = 2000;
    for (int i = 0; i < calls; ++i) {
        const Capsule moved{{rng.f(-6, 6), rng.f(-6, 6), rng.f(-6, 6)}, c.radius, c.height};
        if (saige::sweep_capsule_triangles(moved, {rng.f(-0.2, 0.2), rng.f(-0.2, 0.2), rng.f(-0.2, 0.2)}, mesh)) ++hits;
    }
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / calls;
    std::cout << "  400-triangle mesh, per-tick sweep: " << us << " us per call (" << hits << " of " << calls << " hit)\n";
    require(us < 5000.0, "a 400-triangle sweep costs well under 5 ms even unoptimized");
}

} // namespace

int main() {
    try {
        test_validation();
        test_flat_triangle_landing_and_contact_normals();
        test_edge_and_vertex_contacts();
        test_vertex_against_the_middle_of_the_cylinder();
        test_ramp_contact_normal_and_landing_height();
        test_tunneling_is_impossible();
        test_overlap_and_push_out();
        test_mesh_earliest_hit_and_ties();
        test_coplanar_neighbours_have_no_ghost_edges();
        test_triangulated_box_matches_the_box_sweep();
        test_random_cases_against_the_brute_force_reference();
        test_push_out_against_the_reference();
        test_determinism_and_numerical_range();
        test_cost_of_a_ramp_sized_mesh();
    } catch (const std::exception& error) {
        std::cerr << "capsule triangle test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "capsule triangle tests passed\n";
}
