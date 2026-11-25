#include "saige/static_collision.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using saige::Aabb;
using saige::Capsule;
using saige::StaticCollision;
using saige::StaticPrimitive;
using saige::Triangle;
using saige::Vec3;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Random {
    std::uint64_t state;
    explicit Random(std::uint64_t seed) : state(seed) {}
    double next() {  // [0, 1)
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state >> 11) / 9007199254740992.0;
    }
    float range(double lo, double hi) { return static_cast<float>(lo + (hi - lo) * next()); }
};

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

// ---------------------------------------------------------------------------------------------------------------------------
// The oracles: brute force over every primitive, written independently of the hierarchy.

struct BruteSweep {
    bool hit = false;
    StaticPrimitive::Kind kind{};
    std::size_t index{};
    saige::CapsuleHit detail;
};

BruteSweep brute_sweep(const World& w, const Capsule& c, Vec3 d) {
    BruteSweep out;
    if (const auto b = saige::sweep_capsule_boxes(c, d, w.boxes)) {
        out = {true, StaticPrimitive::Kind::Box, b->index, *b};
    }
    if (const auto t = saige::sweep_capsule_triangles(c, d, w.triangles)) {
        if (!out.hit || t->contact_t < out.detail.contact_t) out = {true, StaticPrimitive::Kind::Triangle, t->index, *t};
    }
    return out;
}

struct BruteRay {
    bool hit = false;
    double t = 0.0;
    std::size_t id = 0;  // boxes first, then triangles
};

BruteRay brute_ray(const World& w, Vec3 origin, Vec3 direction, double max_t) {
    const double len = std::sqrt(static_cast<double>(direction.x) * direction.x + static_cast<double>(direction.y) * direction.y + static_cast<double>(direction.z) * direction.z);
    const double d[3] = {direction.x / len, direction.y / len, direction.z / len};
    const double o[3] = {origin.x, origin.y, origin.z};
    BruteRay best;
    best.t = max_t;
    for (std::size_t i = 0; i < w.boxes.size(); ++i) {
        const double lo[3] = {w.boxes[i].min.x, w.boxes[i].min.y, w.boxes[i].min.z}, hi[3] = {w.boxes[i].max.x, w.boxes[i].max.y, w.boxes[i].max.z};
        double t0 = 0.0, t1 = max_t;
        bool ok = true;
        for (int a = 0; a < 3 && ok; ++a) {
            if (d[a] == 0.0) {
                ok = o[a] >= lo[a] && o[a] <= hi[a];
                continue;
            }
            double a1 = (lo[a] - o[a]) / d[a], a2 = (hi[a] - o[a]) / d[a];
            if (a1 > a2) std::swap(a1, a2);
            t0 = std::max(t0, a1);
            t1 = std::min(t1, a2);
            ok = t0 <= t1;
        }
        if (ok && (t0 < best.t || (t0 == best.t && !best.hit))) best = {true, t0, i};
        else if (ok && t0 == best.t && best.hit && i < best.id) best.id = i;
    }
    for (std::size_t i = 0; i < w.triangles.size(); ++i) {
        const Triangle& tri = w.triangles[i];
        const double a[3] = {tri.a.x, tri.a.y, tri.a.z}, b[3] = {tri.b.x, tri.b.y, tri.b.z}, c[3] = {tri.c.x, tri.c.y, tri.c.z};
        double e1[3], e2[3], p[3], s[3], q[3];
        for (int k = 0; k < 3; ++k) {
            e1[k] = b[k] - a[k];
            e2[k] = c[k] - a[k];
            s[k] = o[k] - a[k];
        }
        const auto cross = [](const double* u, const double* v, double* r) {
            r[0] = u[1] * v[2] - u[2] * v[1];
            r[1] = u[2] * v[0] - u[0] * v[2];
            r[2] = u[0] * v[1] - u[1] * v[0];
        };
        const auto dotp = [](const double* u, const double* v) { return u[0] * v[0] + u[1] * v[1] + u[2] * v[2]; };
        cross(d, e2, p);
        const double det = dotp(e1, p);
        if (std::abs(det) < 1e-14) continue;
        const double u = dotp(s, p) / det;
        if (u < 0.0 || u > 1.0) continue;
        cross(s, e1, q);
        const double v = dotp(d, q) / det;
        if (v < 0.0 || u + v > 1.0) continue;
        const double t = dotp(e2, q) / det;
        if (t < 0.0 || t > max_t) continue;
        const std::size_t id = w.boxes.size() + i;
        if (t < best.t || (t == best.t && !best.hit)) best = {true, t, id};
        else if (t == best.t && best.hit && id < best.id) best.id = id;
    }
    return best;
}

bool same_hit(const saige::StaticCapsuleHit& a, const BruteSweep& b) {
    return a.primitive.kind == b.kind && a.primitive.index == b.index && a.hit.index == b.index && a.hit.t == b.detail.t && a.hit.contact_t == b.detail.contact_t &&
           a.hit.stop == b.detail.stop && a.hit.normal == b.detail.normal && a.hit.point == b.detail.point && a.hit.started_overlapping == b.detail.started_overlapping &&
           a.hit.converged == b.detail.converged;
}

// ---------------------------------------------------------------------------------------------------------------------------

void test_capsule_sweeps_equal_brute_force() {
    Random random(20261124);
    int hits = 0, misses = 0, both_kinds = 0;
    for (int world_index = 0; world_index < 6; ++world_index) {
        const World w = random_world(random, 40 + world_index * 30, 60 + world_index * 40, 30.0);
        const StaticCollision collision(w.boxes, w.triangles);
        saige::QueryScratch scratch;
        for (int i = 0; i < 400; ++i) {
            Capsule c{{random.range(-35, 35), random.range(-35, 35), random.range(-6, 6)}, random.range(0.2, 0.5), 0.0f};
            c.height = c.radius * 2.0f + random.range(0.0, 1.4);
            const double length = random.next() < 0.3 ? random.range(10, 50) : random.range(0.1, 6.0);
            const double az = random.range(0, 6.2831853), el = random.range(-0.5, 0.5);
            const Vec3 d{static_cast<float>(length * std::cos(az) * std::cos(el)), static_cast<float>(length * std::sin(az) * std::cos(el)), static_cast<float>(length * std::sin(el))};
            const BruteSweep want = brute_sweep(w, c, d);
            const auto got = collision.sweep_capsule(c, d, scratch);
            require(got.has_value() == want.hit, "world " + std::to_string(world_index) + " query " + std::to_string(i) + ": hit or miss differs");
            if (got) {
                require(same_hit(*got, want), "world " + std::to_string(world_index) + " query " + std::to_string(i) + ": the hit differs from the brute-force sweep");
                ++hits;
                both_kinds += got->primitive.kind == StaticPrimitive::Kind::Triangle ? 1 : 0;
            } else {
                ++misses;
            }
            // Overlap at the start agrees with the primitives' own push-out.
            bool want_overlap = false;
            for (const Aabb& b : w.boxes) want_overlap = want_overlap || saige::capsule_box_push_out(c, b).has_value();
            for (const Triangle& t : w.triangles) want_overlap = want_overlap || saige::capsule_triangle_push_out(c, t).has_value();
            require(collision.overlaps_capsule(c, scratch) == want_overlap, "overlap differs from brute force");
        }
    }
    require(hits >= 400 && misses >= 400, "too few hits or misses for a meaningful sample: " + std::to_string(hits) + "/" + std::to_string(misses));
    require(both_kinds >= 80, "triangles were hit too rarely: " + std::to_string(both_kinds));
}

void test_rays_equal_brute_force() {
    Random random(77);
    int hits = 0, misses = 0, box_hits = 0, triangle_hits = 0;
    for (int world_index = 0; world_index < 5; ++world_index) {
        const World w = random_world(random, 60 + world_index * 40, 100 + world_index * 60, 35.0);
        const StaticCollision collision(w.boxes, w.triangles);
        for (int i = 0; i < 600; ++i) {
            const Vec3 origin{random.range(-40, 40), random.range(-40, 40), random.range(-6, 6)};
            const Vec3 dir{random.range(-1, 1), random.range(-1, 1), random.range(-0.4, 0.4)};
            if (std::abs(dir.x) + std::abs(dir.y) + std::abs(dir.z) < 1e-3f) continue;
            const float max = random.next() < 0.2 ? 5.0f : 400.0f;
            const BruteRay want = brute_ray(w, origin, dir, max);
            const auto got = collision.raycast(origin, dir, max);
            require(got.has_value() == want.hit, "ray hit or miss differs");
            if (!got) {
                ++misses;
                continue;
            }
            ++hits;
            const std::size_t id = got->primitive.kind == StaticPrimitive::Kind::Box ? got->primitive.index : w.boxes.size() + got->primitive.index;
            box_hits += got->primitive.kind == StaticPrimitive::Kind::Box ? 1 : 0;
            triangle_hits += got->primitive.kind == StaticPrimitive::Kind::Triangle ? 1 : 0;
            require(std::abs(got->distance - want.t) < 1e-9, "ray distance differs from brute force");
            require(id == want.id, "ray hit a different primitive than brute force (same distance " + std::to_string(want.t) + ")");
            const double nl = std::sqrt(static_cast<double>(got->normal.x) * got->normal.x + static_cast<double>(got->normal.y) * got->normal.y + static_cast<double>(got->normal.z) * got->normal.z);
            require(std::abs(nl - 1.0) < 1e-5, "the hit normal is a unit vector");
            const double dl = std::sqrt(static_cast<double>(dir.x) * dir.x + static_cast<double>(dir.y) * dir.y + static_cast<double>(dir.z) * dir.z);
            require(got->distance == 0.0 || (got->normal.x * dir.x + got->normal.y * dir.y + got->normal.z * dir.z) / dl <= 1e-6, "the normal faces the origin");
        }
    }
    require(hits >= 500 && misses >= 500 && box_hits >= 100 && triangle_hits >= 100, "rays: too small a sample of each outcome");
}

void test_specific_cases() {
    const std::vector<Aabb> boxes{{{-1, -1, 0}, {1, 1, 2}}, {{-1, -1, 0}, {1, 1, 2}}, {{5, -1, 0}, {7, 1, 2}}};  // two identical boxes
    const std::vector<Triangle> tris{{{3, -2, 0}, {3, 2, 0}, {3, 0, 4}}};
    const StaticCollision c(boxes, tris);
    require(c.box_count() == 3 && c.triangle_count() == 1 && c.node_count() >= 1 && c.depth() >= 1, "counts");

    // A ray along +x from the left: hits the first of the two identical boxes (a genuine tie goes to the lower index).
    const auto first = c.raycast({-5, 0, 1}, {1, 0, 0}, 100);
    require(first && first->primitive == StaticPrimitive{StaticPrimitive::Kind::Box, 0} && std::abs(first->distance - 4.0) < 1e-9, "tie goes to the lower index");
    require(first->normal == Vec3{-1, 0, 0} && first->point == Vec3{-1, 0, 1}, "entry face and point");
    // The same ray, scaled direction and a limit that stops short.
    require(c.raycast({-5, 0, 1}, {40, 0, 0}, 100)->distance == first->distance, "the direction is normalized");
    require(!c.raycast({-5, 0, 1}, {1, 0, 0}, 3.9f), "a limit before the box finds nothing");
    require(c.raycast({-5, 0, 1}, {1, 0, 0}, 4.0f).has_value(), "a limit exactly at the box finds it");
    // From inside a box: distance 0, the normal opposes the ray.
    const auto inside = c.raycast({0, 0, 1}, {0, 1, 0}, 10);
    require(inside && inside->distance == 0.0 && inside->normal == Vec3{0, -1, 0}, "starting inside a box hits at distance 0");
    // The triangle is two-sided: from +x and from -x the normal faces the ray.
    const auto tri_from_right = c.raycast({10, 0.2f, 1}, {-1, 0, 0}, 3);  // stops before the box at x = 7? it starts at 10: hits the box at 7 first
    require(tri_from_right && tri_from_right->primitive.kind == StaticPrimitive::Kind::Box, "the nearer box is hit before the triangle behind it");
    const auto tri_hit = c.raycast({4.5f, 0.2f, 1}, {-1, 0, 0}, 10);
    require(tri_hit && tri_hit->primitive.kind == StaticPrimitive::Kind::Triangle && tri_hit->normal.x > 0.99f, "two-sided triangle, normal toward the origin");
    const auto tri_other = c.raycast({2, 0.2f, 1}, {1, 0, 0}, 10);
    require(tri_other && tri_other->primitive.kind == StaticPrimitive::Kind::Triangle && tri_other->normal.x < -0.99f, "and from the other side");

    // A capsule sweep tie between two identical boxes goes to the lower index; a box wins an exact tie against nothing else here.
    const Capsule cap{{-6, 0, 0}, 0.3f, 1.7f};
    const auto sweep = c.sweep_capsule(cap, {10, 0, 0});
    require(sweep && sweep->primitive == StaticPrimitive{StaticPrimitive::Kind::Box, 0}, "sweep tie goes to the lower index");

    // Region query: ascending ids, boxes before triangles.
    std::vector<StaticPrimitive> found;
    c.query_region({{-2, -3, -1}, {4, 3, 5}}, found);
    require(found.size() == 3 && found[0].index == 0 && found[1].index == 1 && found[1].kind == StaticPrimitive::Kind::Box && found[2].kind == StaticPrimitive::Kind::Triangle,
            "region query order");
    found.clear();
    c.query_region({{100, 100, 100}, {101, 101, 101}}, found);
    require(found.empty(), "a distant region finds nothing");

    // Empty geometry: no hits, but the capsule envelope is still enforced; invalid input throws.
    const StaticCollision empty;
    require(!empty.raycast({0, 0, 0}, {1, 0, 0}, 10) && !empty.sweep_capsule(cap, {1, 0, 0}) && !empty.overlaps_capsule(cap), "empty collision hits nothing");
    const auto throws = [](auto&& fn) {
        try {
            fn();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    require(throws([&] { (void)empty.sweep_capsule({{-1e8f, 0, 0}, 0.3f, 1.7f}, {2e8f, 0, 0}); }), "a sweep outside the envelope throws even with no geometry");
    require(throws([&] { (void)c.sweep_capsule(cap, {nan, 0, 0}); }), "a non-finite displacement throws");
    require(throws([&] { (void)c.raycast({0, 0, 0}, {0, 0, 0}, 1); }) && throws([&] { (void)c.raycast({nan, 0, 0}, {1, 0, 0}, 1); }) &&
                throws([&] { (void)c.raycast({0, 0, 0}, {1, 0, 0}, -1); }) && throws([&] { (void)c.raycast({0, 0, 0}, {1, 0, 0}, nan); }),
            "invalid rays throw");
    const std::vector<Aabb> bad_box{{{0, 0, 0}, {0, 1, 1}}};
    const std::vector<Triangle> bad_tri{{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}};
    require(throws([&] { StaticCollision x(bad_box, {}); }) && throws([&] { StaticCollision x({}, bad_tri); }), "invalid geometry is rejected at build");
    // Geometry may extend beyond the capsule envelope: a ray reaches a far box.
    const std::vector<Aabb> far{{{1e6f, -1, -1}, {1e6f + 2, 1, 1}}};
    const StaticCollision far_collision(far, {});
    require(far_collision.raycast({0, 0, 0}, {1, 0, 0}, 2e6f) && std::abs(far_collision.raycast({0, 0, 0}, {1, 0, 0}, 2e6f)->distance - 1e6) < 1.0, "far geometry is queryable by ray");
}

void test_region_query_equals_brute_force_and_build_is_deterministic() {
    Random random(5);
    const World w = random_world(random, 120, 200, 50.0);
    const StaticCollision a(w.boxes, w.triangles), b(w.boxes, w.triangles);
    require(a.node_count() == b.node_count() && a.depth() == b.depth(), "the same inputs build the same tree");
    for (int i = 0; i < 300; ++i) {
        const Vec3 c{random.range(-55, 55), random.range(-55, 55), random.range(-10, 10)};
        const Vec3 h{random.range(0.2, 8), random.range(0.2, 8), random.range(0.2, 8)};
        const Aabb region{{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z + h.z}};
        std::vector<StaticPrimitive> got, again, want;
        a.query_region(region, got);
        b.query_region(region, again);
        require(got == again, "equal builds answer equally");
        const auto overlaps = [&](const float lo[3], const float hi[3]) {
            return lo[0] <= region.max.x && hi[0] >= region.min.x && lo[1] <= region.max.y && hi[1] >= region.min.y && lo[2] <= region.max.z && hi[2] >= region.min.z;
        };
        for (std::size_t k = 0; k < w.boxes.size(); ++k) {
            const float lo[3] = {w.boxes[k].min.x, w.boxes[k].min.y, w.boxes[k].min.z}, hi[3] = {w.boxes[k].max.x, w.boxes[k].max.y, w.boxes[k].max.z};
            if (overlaps(lo, hi)) want.push_back({StaticPrimitive::Kind::Box, k});
        }
        for (std::size_t k = 0; k < w.triangles.size(); ++k) {
            const Triangle& t = w.triangles[k];
            const float lo[3] = {std::min({t.a.x, t.b.x, t.c.x}), std::min({t.a.y, t.b.y, t.c.y}), std::min({t.a.z, t.b.z, t.c.z})};
            const float hi[3] = {std::max({t.a.x, t.b.x, t.c.x}), std::max({t.a.y, t.b.y, t.c.y}), std::max({t.a.z, t.b.z, t.c.z})};
            if (overlaps(lo, hi)) want.push_back({StaticPrimitive::Kind::Triangle, k});
        }
        require(got == want, "region query differs from brute force at query " + std::to_string(i));
    }
}

void test_scaling_is_logarithmic_in_practice() {
    // Not a timing assertion (CI machines vary): the tree over 20000 primitives is shallow and a sweep gathers few candidates.
    Random random(31337);
    const World w = random_world(random, 4000, 16000, 400.0);
    const StaticCollision collision(w.boxes, w.triangles);
    require(collision.depth() <= 40, "the tree is shallow: " + std::to_string(collision.depth()));
    saige::QueryScratch scratch;
    std::size_t candidates = 0;
    constexpr int queries = 200;
    for (int i = 0; i < queries; ++i) {
        const Capsule c{{random.range(-250, 250), random.range(-250, 250), random.range(-20, 20)}, 0.3f, 1.7f};
        (void)collision.sweep_capsule(c, {random.range(-2, 2), random.range(-2, 2), 0.0f}, scratch);
        candidates += scratch.ids.size();
    }
    require(candidates / queries < 100, "a local sweep examines few primitives out of 20000: " + std::to_string(candidates / queries));

    const auto time = [&](auto&& body) {
        const auto start = std::chrono::steady_clock::now();
        body();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    };
    Random q1(9), q2(9);
    const double indexed = time([&] {
        for (int i = 0; i < queries; ++i) {
            const Capsule c{{q1.range(-250, 250), q1.range(-250, 250), q1.range(-20, 20)}, 0.3f, 1.7f};
            (void)collision.sweep_capsule(c, {q1.range(-2, 2), q1.range(-2, 2), 0.0f}, scratch);
        }
    });
    const double brute = time([&] {
        for (int i = 0; i < queries; ++i) {
            const Capsule c{{q2.range(-250, 250), q2.range(-250, 250), q2.range(-20, 20)}, 0.3f, 1.7f};
            (void)brute_sweep(w, c, {q2.range(-2, 2), q2.range(-2, 2), 0.0f});
        }
    });
    std::cout << "  20000 primitives, " << queries << " local sweeps: indexed " << indexed << " ms, brute force " << brute << " ms (informational, software CPU)\n";
}

} // namespace

void test_ray_modes() {
    using saige::RayMode;
    const StaticCollision world(std::vector<Aabb>{{{0, 0, 0}, {1, 1, 1}}, {{-5, 0, 0}, {-4, 1, 1}}}, {});
    const Vec3 on_min_face{0, 0.5f, 0.5f};
    // Solid (the default) is unchanged: an origin on the boundary is in the closed box whatever its direction.
    auto solid = world.raycast(on_min_face, {-1, 0, 0}, 10.0f);
    require(solid && solid->distance == 0.0 && solid->primitive.index == 0 && !solid->started_inside, "Solid: a boundary origin leaving a box still hits it at 0, not 'inside'");
    require(world.raycast(on_min_face, {-1, 0, 0}, 10.0f, RayMode::Solid)->primitive.index == 0, "the explicit Solid mode is the default");
    require(solid->normal.x == 1.0f && solid->normal.y == 0.0f && solid->normal.z == 0.0f, "Solid: a boundary origin's normal is the reverse of the direction, as before");
    // Surface skips what the ray only touches and finds what lies beyond.
    auto surface = world.raycast(on_min_face, {-1, 0, 0}, 10.0f, RayMode::Surface);
    require(surface && surface->primitive.index == 1 && std::fabs(surface->distance - 4.0) < 1e-9 && surface->normal.x == 1.0f, "Surface: leaving a face finds the box beyond, entered at its max face");
    require(!world.raycast(on_min_face, {-1, 0, 0}, 3.0f, RayMode::Surface), "Surface: nothing within the length is a miss");
    auto in = world.raycast(on_min_face, {1, 0, 0}, 10.0f, RayMode::Surface);
    require(in && in->distance == 0.0 && in->normal.x == -1.0f && !in->started_inside, "Surface: moving in through a face hits it at 0 with the face's outward normal, not 'inside'");
    auto inside = world.raycast({0.5f, 0.5f, 0.5f}, {0, 1, 0}, 10.0f, RayMode::Surface);
    require(inside && inside->distance == 0.0 && inside->started_inside && inside->normal.y == -1.0f, "Surface: strictly inside is started_inside with the reverse direction");
    // An edge origin picks the entry face the direction is most aligned with: on the min-X/min-Y edge moving (1, 0.25, 0) enters through min X.
    auto edge = world.raycast({0, 0, 0.5f}, {1, 0.25f, 0}, 10.0f, RayMode::Surface);
    require(edge && edge->distance == 0.0 && edge->normal.x == -1.0f, "Surface: an edge origin enters through the face it moves in through most");
    // A shallow entry: the direction is mostly along the face, so its component through the face is small (0.2 / |d|), but it is still the face it enters through.
    auto shallow = world.raycast({0, 0.5f, 0.2f}, {0.2f, 0, 1}, 10.0f, RayMode::Surface);
    require(shallow && shallow->distance == 0.0 && shallow->normal.x == -1.0f && shallow->normal.z == 0.0f, "Surface: a shallow entry still reports the face it enters through");
    auto tie = world.raycast({0, 0, 0.5f}, {1, 1, 0}, 10.0f, RayMode::Surface);
    require(tie && tie->normal.x == -1.0f && tie->normal.y == 0.0f, "an exact tie between faces goes to the lowest axis");
    auto edge_y = world.raycast({0, 0, 0.5f}, {0.25f, 1, 0}, 10.0f, RayMode::Surface);
    require(edge_y && edge_y->normal.y == -1.0f, "and through the other face when the direction favours it");
    auto edge_hi = world.raycast({1, 1, 0.5f}, {-0.25f, -1, 0}, 10.0f, RayMode::Surface);
    require(edge_hi && edge_hi->normal.y == 1.0f, "a max-face origin enters through the max face (outward normal +)");
}

int main() {
    try {
        test_specific_cases();
        test_ray_modes();
        test_capsule_sweeps_equal_brute_force();
        test_rays_equal_brute_force();
        test_region_query_equals_brute_force_and_build_is_deterministic();
        test_scaling_is_logarithmic_in_practice();
    } catch (const std::exception& error) {
        std::cerr << "static collision test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "static collision tests passed\n";
}
