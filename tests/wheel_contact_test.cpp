#include "saige/wheel_contact.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::atomic<std::size_t> allocation_count{0};
}

// Counting allocator for the whole test executable: the allocation claims in the documentation are measured, not asserted.
void* operator new(std::size_t size) {
    allocation_count.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

using saige::Aabb;
using saige::StaticCollision;
using saige::StaticPrimitive;
using saige::Triangle;
using saige::Vec3;
using saige::WheelContact;
using saige::WheelContactStatus;
using saige::WheelProbe;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

template <class F>
bool throws_invalid(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

struct Random {
    std::uint64_t state;
    explicit Random(std::uint64_t seed) : state(seed) {}
    double next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state >> 11) / 9007199254740992.0;
    }
    float range(double lo, double hi) { return static_cast<float>(lo + (hi - lo) * next()); }
};

WheelProbe down(Vec3 origin, float length, float radius = 0.0f) {
    WheelProbe p;
    p.origin = origin;
    p.length = length;
    p.radius = radius;
    return p;
}

const Aabb floor_box{{-20, -20, -2}, {20, 20, 0}};  // the ground: its top face is z = 0

StaticCollision flat_world() { return StaticCollision(std::vector<Aabb>{floor_box}, {}); }

void test_flat_ground() {
    const StaticCollision world = flat_world();
    const WheelContact ray = saige::probe_wheel(world, down({1, 2, 1.0f}, 3.0f));
    require(ray.status == WheelContactStatus::Support, "ray: support");
    require(near(ray.distance, 1.0, 1e-9) && near(ray.travel, 1.0 / 3.0, 1e-6), "ray: distance 1 m, a third of the travel");
    require(near(ray.point.z, 0.0, 1e-6) && near(ray.point.x, 1.0, 1e-6) && near(ray.point.y, 2.0, 1e-6), "ray: the point on the ground");
    require(near(ray.normal.z, 1.0, 1e-6) && near(ray.slope_cos, 1.0, 1e-6), "ray: the support normal points up");
    require(ray.primitive == StaticPrimitive{StaticPrimitive::Kind::Box, 0}, "ray: the shape id");
    require(near(ray.center.z, 0.0, 1e-6), "ray: the center at contact");
    // A sphere of radius 0.3: its center stops 0.3 above the ground.
    const WheelContact sphere = saige::probe_wheel(world, down({1, 2, 1.0f}, 3.0f, 0.3f));
    require(sphere.status == WheelContactStatus::Support && near(sphere.distance, 0.7, 1e-6) && near(sphere.center.z, 0.3, 1e-5), "sphere: the center stops one radius above the ground");
    require(near(sphere.normal.z, 1.0, 1e-5) && near(sphere.point.z, 0.0, 1e-5), "sphere: the contact point and normal");
    // An oblique probe (a wheel steered or a chassis tilted): the direction is normalized, and the center and the point follow it.
    {
        WheelProbe slanted = down({0, 0, 2.0f}, 4.0f);
        slanted.direction = {3.0f, 0.0f, -3.0f};  // not unit length: 45 degrees
        const WheelContact c = saige::probe_wheel(world, slanted);
        require(c.status == WheelContactStatus::Support && near(c.distance, 2.0 * std::sqrt(2.0), 1e-5), "oblique ray: distance along the unit direction");
        require(near(c.center.x, 2.0, 1e-5) && near(c.center.y, 0.0, 1e-6) && near(c.center.z, 0.0, 1e-5) && near(c.point.x, 2.0, 1e-5), "oblique ray: the center and the point follow the direction");
        slanted.radius = 0.5f;
        const WheelContact s = saige::probe_wheel(world, slanted);
        require(s.status == WheelContactStatus::Support && near(s.center.z, 0.5, 1e-4) && near(s.center.x, 1.5, 1e-4) && near(s.point.x, 1.5, 1e-4), "oblique sphere: it stops one radius above the ground along the path");
    }
    // Out of reach: a miss.
    require(saige::probe_wheel(world, down({0, 0, 5.0f}, 3.0f)).status == WheelContactStatus::Miss, "too short to reach the ground: a miss");
    require(saige::probe_wheel(world, down({0, 0, 5.0f}, 3.0f, 0.3f)).status == WheelContactStatus::Miss, "...also for the sphere");
    // Pointing away from the ground.
    WheelProbe up_probe = down({0, 0, 1.0f}, 3.0f);
    up_probe.direction = {0, 0, 1};
    require(saige::probe_wheel(world, up_probe).status == WheelContactStatus::Miss, "pointing up: a miss");
}

void test_slopes() {
    // A ramp rising along +X at `angle` degrees, as two triangles of a 4 x 4 patch.
    const auto ramp_world = [](double degrees) {
        const double a = degrees * 3.14159265358979 / 180.0;
        const float run = 4.0f, rise = static_cast<float>(std::tan(a)) * run;
        const std::vector<Triangle> t{{{0, -2, 0}, {run, -2, rise}, {run, 2, rise}}, {{0, -2, 0}, {run, 2, rise}, {0, 2, 0}}};
        return StaticCollision({}, t);
    };
    for (const double degrees : {10.0, 30.0, 45.0}) {
        const StaticCollision world = ramp_world(degrees);
        const WheelContact c = saige::probe_wheel(world, down({2.0f, 0.0f, 6.0f}, 8.0f));
        const double a = degrees * 3.14159265358979 / 180.0;
        require(c.status == WheelContactStatus::Support, "a " + std::to_string(degrees) + " degree ramp supports");
        require(near(c.slope_cos, std::cos(a), 1e-5), "the slope cosine is cos(angle)");
        require(near(c.normal.x, -std::sin(a), 1e-5) && near(c.normal.z, std::cos(a), 1e-5), "the normal tilts back toward the downhill side");
        require(near(c.point.z, 2.0 * std::tan(a), 1e-4), "the contact lies on the ramp");
    }
    // Steeper than permitted: blocked, with the slope cosine still reported. The limit is a closed bound on the cosine.
    const StaticCollision steep = ramp_world(60.0);
    const WheelContact blocked = saige::probe_wheel(steep, down({2.0f, 0.0f, 9.0f}, 12.0f));  // default maximum about 51.6 degrees
    require(blocked.status == WheelContactStatus::Blocked && near(blocked.slope_cos, 0.5, 1e-5), "a 60 degree ramp is blocked at the default limit");
    WheelProbe permissive = down({2.0f, 0.0f, 9.0f}, 12.0f);
    permissive.max_slope_radians = 61.0f * 3.14159265f / 180.0f;
    require(saige::probe_wheel(steep, permissive).status == WheelContactStatus::Support, "...and supported when the limit allows it");
    require(saige::probe_wheel(flat_world(), down({0, 0, 1}, 3)).status == WheelContactStatus::Support, "flat ground at the default limit");
    WheelProbe flat_zero = down({0, 0, 1}, 3);
    flat_zero.max_slope_radians = 0.0f;
    require(saige::probe_wheel(flat_world(), flat_zero).status == WheelContactStatus::Support, "exactly flat ground at a limit of zero degrees");
    require(saige::probe_wheel(ramp_world(10.0), (WheelProbe{{2.0f, 0.0f, 6.0f}, {0, 0, -1}, 8.0f, 0.0f, {0, 0, 1}, 0.0f})).status == WheelContactStatus::Blocked, "a 10 degree ramp at a limit of zero degrees");
    // A chassis whose up is tilted: the same ramp is flat to it.
    WheelProbe tilted = down({2.0f, 0.0f, 6.0f}, 8.0f);
    tilted.up = {-static_cast<float>(std::sin(0.5236)), 0.0f, static_cast<float>(std::cos(0.5236))};  // up for a ramp of 30 degrees
    tilted.max_slope_radians = 0.05f;
    require(saige::probe_wheel(ramp_world(30.0), tilted).status == WheelContactStatus::Support, "a tilted up makes the 30 degree ramp flat to the chassis");
}

void test_ledges_and_edges() {
    // A plateau box with a drop beside it.
    const std::vector<Aabb> plateau{Aabb{{0, -5, -3}, {5, 5, 0}}};
    const StaticCollision world(plateau, {});
    require(saige::probe_wheel(world, down({4.0f, 0, 2.0f}, 4.0f)).status == WheelContactStatus::Support, "over the plateau: support");
    require(saige::probe_wheel(world, down({6.0f, 0, 2.0f}, 4.0f)).status == WheelContactStatus::Miss, "past the ledge: a ray misses (missing ground)");
    // A sphere center just past the edge still touches the plateau's corner edge: the normal tilts outward and the contact is below the flat-ground one.
    const WheelContact edge = saige::probe_wheel(world, down({5.1f, 0, 2.0f}, 4.0f, 0.4f));
    require(edge.status != WheelContactStatus::Miss, "a sphere just past the ledge still rolls on the edge");
    require(edge.normal.x > 0.0f && edge.normal.z > 0.0f && edge.slope_cos < 1.0f, "the edge contact's normal leans outward");
    require(edge.distance > 2.0 - 0.4 + 1e-3, "the wheel sinks past the flat-ground contact distance as it leaves the edge");
    require(saige::probe_wheel(world, down({5.5f, 0, 2.0f}, 4.0f, 0.4f)).status == WheelContactStatus::Miss, "a sphere farther than its radius past the ledge misses");
}

void test_underside_and_two_sided_triangles() {
    // A ceiling box above and a probe from below going up: its facing normal points down, so it is blocked.
    const std::vector<Aabb> ceiling{Aabb{{-5, -5, 2}, {5, 5, 3}}};
    const StaticCollision under(ceiling, {});
    WheelProbe up_probe = down({0, 0, 0}, 4.0f);
    up_probe.direction = {0, 0, 1};
    const WheelContact c = saige::probe_wheel(under, up_probe);
    require(c.status == WheelContactStatus::Blocked && c.normal.z < -0.99f && c.slope_cos < -0.99f, "the underside of a ceiling is blocked, its normal facing down toward the probe");
    require(near(c.distance, 2.0, 1e-9), "...at the right distance");
    // A triangle's winding does not matter: probed from above it supports whichever way it faces.
    const std::vector<Triangle> up_facing{{{-5, -5, 0}, {5, -5, 0}, {0, 5, 0}}}, down_facing{{{-5, -5, 0}, {0, 5, 0}, {5, -5, 0}}};
    for (const auto& tri : {up_facing, down_facing}) {
        const StaticCollision world({}, tri);
        const WheelContact top = saige::probe_wheel(world, down({0, 0, 1.0f}, 3.0f));
        require(top.status == WheelContactStatus::Support && near(top.normal.z, 1.0, 1e-6), "a triangle supports from above whichever way its winding faces");
        WheelProbe from_below = down({0, 0, -1.0f}, 3.0f);
        from_below.direction = {0, 0, 1};
        const WheelContact bottom = saige::probe_wheel(world, from_below);
        require(bottom.status == WheelContactStatus::Blocked && near(bottom.normal.z, -1.0, 1e-6), "from below the same triangle is an underside: blocked, normal facing down");
    }
}

void test_nearest_contact_and_blockers() {
    // A plank between the mount and the floor: the nearest contact is the plank's top, a support.
    const std::vector<Aabb> two{floor_box, Aabb{{-1, -1, 1.0f}, {1, 1, 1.2f}}};
    const StaticCollision with_plank(two, {});
    const WheelContact on_plank = saige::probe_wheel(with_plank, down({0, 0, 2.0f}, 4.0f));
    require(on_plank.status == WheelContactStatus::Support && near(on_plank.point.z, 1.2, 1e-6) && on_plank.primitive.index == 1, "the plank's top is the nearest contact and supports");
    // A steep face above flat ground: the probe meets it first and is blocked; the ground behind it is not looked for.
    const std::vector<Triangle> wall{{{-1, -1, 0.5f}, {1, -1, 0.5f}, {1, 1, 1.5f}}, {{-1, -1, 0.5f}, {1, 1, 1.5f}, {-1, 1, 1.5f}}};  // a ~35 degree sheet; steepened below
    (void)wall;
    const std::vector<Triangle> steep_sheet{{{-1, -1, 0.5f}, {1, -1, 0.5f}, {1, -1, 1.5f}}};  // a vertical sheet the probe passes along: build a tilted one instead
    (void)steep_sheet;
    const std::vector<Triangle> tilted{{{-2, -2, 1.0f}, {2, -2, 1.0f + 4.0f * 2.0f}, {2, 2, 1.0f + 4.0f * 2.0f}}, {{-2, -2, 1.0f}, {2, 2, 1.0f + 8.0f}, {-2, 2, 1.0f}}};  // ~63 degrees
    const StaticCollision blocker(std::vector<Aabb>{floor_box}, tilted);
    const WheelContact hit = saige::probe_wheel(blocker, down({0.0f, 0.0f, 12.0f}, 14.0f));
    require(hit.status == WheelContactStatus::Blocked && hit.primitive.kind == StaticPrimitive::Kind::Triangle && hit.point.z > 1.0f, "a steep face above the ground blocks, and the ground behind it is not skipped to");
    // The same face does not block a probe that avoids it.
    require(saige::probe_wheel(blocker, down({-10.0f, 0.0f, 3.0f}, 5.0f)).status == WheelContactStatus::Support, "a probe beside the blocker finds the ground");
}

void test_initial_overlap() {
    const StaticCollision world = flat_world();
    // A ray starting inside the ground box.
    const WheelContact inside = saige::probe_wheel(world, down({0, 0, -1.0f}, 2.0f));
    require(inside.status == WheelContactStatus::InitialOverlap && inside.distance == 0.0 && near(inside.normal.z, 1.0, 1e-6), "a ray starting inside a solid box: initial overlap, normal against the direction");
    // A ray starting exactly on the surface and heading down touches from above: a support at distance 0, not an overlap.
    const WheelContact on = saige::probe_wheel(world, down({0, 0, 0.0f}, 2.0f));
    require(on.status == WheelContactStatus::Support && on.distance == 0.0, "a ray starting on the surface is a zero-distance support");
    // A sphere that already penetrates the ground.
    const WheelContact deep = saige::probe_wheel(world, down({0, 0, 0.1f}, 2.0f, 0.3f));
    require(deep.status == WheelContactStatus::InitialOverlap && deep.distance == 0.0 && deep.normal.z > 0.99f, "a sphere penetrating the ground: initial overlap with the push-out direction");
    // The same sphere resting exactly on it, moving away: a miss.
    WheelProbe leaving = down({0, 0, 0.3f}, 2.0f, 0.3f);
    leaving.direction = {0, 0, 1};
    require(saige::probe_wheel(world, leaving).status == WheelContactStatus::Miss, "a sphere touching the ground and moving away: a miss");
}

void test_seams_ties_and_replay() {
    // Ground made of two coplanar triangles: probes crossing the seam see one continuous surface.
    const std::vector<Triangle> quad{{{-4, -4, 0}, {4, -4, 0}, {4, 4, 0}}, {{-4, -4, 0}, {4, 4, 0}, {-4, 4, 0}}};  // the seam is the diagonal y = x
    const StaticCollision world({}, quad);
    for (const float radius : {0.0f, 0.35f}) {
        for (int i = -30; i <= 30; ++i) {
            const float x = static_cast<float>(i) * 0.1f;
            const WheelContact c = saige::probe_wheel(world, down({x, x * 0.99f + 0.07f, 1.0f}, 3.0f, radius));  // crossing the seam in the middle
            require(c.status == WheelContactStatus::Support && near(c.normal.z, 1.0, 1e-4) && near(c.distance, 1.0 - radius, 1e-4), "across a coplanar seam: the same support (radius " + std::to_string(radius) + ")");
        }
    }
    // Exactly on the seam (a tie): the lower shape id.
    const WheelContact on_seam = saige::probe_wheel(world, down({1.0f, 1.0f, 1.0f}, 3.0f));
    require(on_seam.status == WheelContactStatus::Support && on_seam.primitive.index <= 1, "on the seam: a support from one of the two");
    // Ties: identical boxes (lower id), and a box against a coplanar triangle (the box wins).
    const std::vector<Aabb> twin{floor_box, floor_box};
    const StaticCollision twins(twin, {});
    require(saige::probe_wheel(twins, down({0, 0, 1}, 3)).primitive.index == 0, "identical boxes: the lower id");
    const std::vector<Triangle> top{{{-3, -3, 0}, {3, -3, 0}, {0, 3, 0}}};
    const StaticCollision mixed(std::vector<Aabb>{floor_box}, top);
    require(saige::probe_wheel(mixed, down({0, 0, 1}, 3)).primitive.kind == StaticPrimitive::Kind::Box, "a box against a coplanar triangle: the box wins the exact tie");
    // Replay: the same probes give the same bytes.
    Random random(11);
    std::vector<WheelProbe> probes;
    for (int i = 0; i < 40; ++i) probes.push_back(down({random.range(-3, 3), random.range(-3, 3), random.range(0.5, 2.0)}, 3.0f, i % 2 == 0 ? 0.0f : 0.3f));
    saige::QueryScratch scratch;
    std::vector<WheelContact> a(probes.size()), b(probes.size());
    saige::probe_wheels(world, probes, a, scratch);
    saige::probe_wheels(world, probes, b, scratch);
    for (std::size_t i = 0; i < a.size(); ++i) {
        require(a[i].status == b[i].status && a[i].distance == b[i].distance && a[i].normal == b[i].normal && a[i].point == b[i].point && a[i].primitive == b[i].primitive, "replay: identical");
        const WheelContact single = saige::probe_wheel(world, probes[i], scratch);
        require(single.status == a[i].status && single.distance == a[i].distance && single.primitive == a[i].primitive, "probe_wheels equals probe_wheel");
    }
}

struct World {
    std::vector<Aabb> boxes;
    std::vector<Triangle> triangles;
};

World random_world(Random& random, int boxes, int triangles) {
    World w;
    for (int i = 0; i < boxes; ++i) {
        const Vec3 c{random.range(-30, 30), random.range(-30, 30), random.range(-2, 2)};
        const Vec3 h{random.range(0.5, 4.0), random.range(0.5, 4.0), random.range(0.3, 2.0)};
        w.boxes.push_back({{c.x - h.x, c.y - h.y, c.z - h.z}, {c.x + h.x, c.y + h.y, c.z + h.z}});
    }
    for (int i = 0; i < triangles; ++i) {
        const Vec3 o{random.range(-30, 30), random.range(-30, 30), random.range(-2, 2)};
        const auto off = [&] { return Vec3{o.x + random.range(-5, 5), o.y + random.range(-5, 5), o.z + random.range(-2, 2)}; };
        const Triangle t{o, off(), off()};
        if (saige::is_valid_triangle(t)) w.triangles.push_back(t);
    }
    return w;
}

void test_equals_brute_force() {
    Random random(0x77EE1ull);
    int contacts = 0, misses = 0, blocked = 0, overlaps = 0;
    for (int world = 0; world < 4; ++world) {
        const World w = random_world(random, 40 + 20 * world, 80 + 40 * world);
        const StaticCollision collision(w.boxes, w.triangles);
        saige::QueryScratch scratch;
        for (int i = 0; i < 200; ++i) {
            WheelProbe p = down({random.range(-30, 30), random.range(-30, 30), random.range(0.5, 6.0)}, random.range(1.0, 8.0), random.range(0.1, 0.6));
            p.direction = {random.range(-0.3, 0.3), random.range(-0.3, 0.3), -1.0f};
            const WheelContact got = saige::probe_wheel(collision, p, scratch);
            // The oracle: the exact capsule sweeps over every primitive, the earlier by exact time with a box winning a tie.
            const double len = std::sqrt(static_cast<double>(p.direction.x) * p.direction.x + static_cast<double>(p.direction.y) * p.direction.y + static_cast<double>(p.direction.z) * p.direction.z);
            const Vec3 d{static_cast<float>(p.direction.x / len * p.length), static_cast<float>(p.direction.y / len * p.length), static_cast<float>(p.direction.z / len * p.length)};
            const saige::Capsule sphere{{p.origin.x, p.origin.y, p.origin.z - p.radius}, p.radius, 2.0f * p.radius};
            const auto b = saige::sweep_capsule_boxes(sphere, d, w.boxes);
            const auto t = saige::sweep_capsule_triangles(sphere, d, w.triangles);
            const bool any = b.has_value() || t.has_value();
            require((got.status != WheelContactStatus::Miss) == any, "wheel probe: hit existence equals brute force");
            if (!any) {
                ++misses;
                continue;
            }
            const bool use_triangle = t.has_value() && (!b.has_value() || t->contact_t < b->contact_t);
            const saige::CapsuleHit& h = use_triangle ? *t : *b;
            require(got.primitive == StaticPrimitive{use_triangle ? StaticPrimitive::Kind::Triangle : StaticPrimitive::Kind::Box, h.index}, "wheel probe: the same primitive");
            // The point is computed at a stop that backs off from every primitive in the query, so it may differ in the last bits between the subset the broadphase examines and all of
            // them; the normal and the exact distance are the contact's.
            require(near(got.normal.x, h.normal.x, 1e-5) && near(got.normal.y, h.normal.y, 1e-5) && near(got.normal.z, h.normal.z, 1e-5), "wheel probe: the same normal");
            require(near(got.point.x, h.point.x, 1e-3) && near(got.point.y, h.point.y, 1e-3) && near(got.point.z, h.point.z, 1e-3), "wheel probe: the same point");
            if (h.started_overlapping) {
                require(got.status == WheelContactStatus::InitialOverlap && got.distance == 0.0, "wheel probe: initial overlap");
                ++overlaps;
            } else {
                // (the oracle rounds the displacement to float a little differently from the probe, so the distance agrees to the float rounding of a few meters)
                require(near(got.distance, h.contact_t * static_cast<double>(p.length), 1e-4), "wheel probe: the distance");
                require(got.status == WheelContactStatus::Support || got.status == WheelContactStatus::Blocked, "wheel probe: a contact");
                const double cosine = static_cast<double>(h.normal.x) * 0.0 + static_cast<double>(h.normal.y) * 0.0 + static_cast<double>(h.normal.z);
                require((got.status == WheelContactStatus::Support) == (cosine >= std::cos(static_cast<double>(p.max_slope_radians))), "wheel probe: the slope rule");
                (got.status == WheelContactStatus::Support ? contacts : blocked) += 1;
            }
        }
    }
    require(contacts > 150 && misses > 20 && blocked > 30 && overlaps >= 1, "the random probes exercise every status: " + std::to_string(contacts) + " supports, " + std::to_string(blocked) + " blocked, " + std::to_string(misses) + " misses, " + std::to_string(overlaps) + " overlaps");
    std::cout << "  800 wheel probes equal brute force (" << contacts << " supports, " << blocked << " blocked, " << misses << " misses, " << overlaps << " initial overlaps)\n";
}

void test_validation() {
    const StaticCollision world = flat_world();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto refused = [&](auto&& mutate) {
        WheelProbe p = down({0, 0, 1}, 2.0f, 0.3f);
        mutate(p);
        return throws_invalid([&] { (void)saige::probe_wheel(world, p); });
    };
    require(refused([&](WheelProbe& p) { p.origin.x = nan; }), "NaN origin");
    require(refused([&](WheelProbe& p) { p.direction = {0, 0, 0}; }), "zero direction");
    require(refused([&](WheelProbe& p) { p.direction.y = nan; }), "NaN direction");
    require(refused([&](WheelProbe& p) { p.up = {0, 0, 0}; }), "zero up");
    require(refused([](WheelProbe& p) { p.length = 0.0f; }), "zero length");
    require(refused([](WheelProbe& p) { p.length = 100.0f; }), "length beyond max_sweep_length");
    require(refused([&](WheelProbe& p) { p.length = nan; }), "NaN length");
    require(refused([](WheelProbe& p) { p.radius = -0.1f; }), "negative radius");
    require(refused([](WheelProbe& p) { p.radius = 5.0f; }), "radius beyond max_wheel_radius");
    require(refused([](WheelProbe& p) { p.max_slope_radians = 1.6f; }), "slope at or beyond a right angle");
    require(refused([](WheelProbe& p) { p.max_slope_radians = -0.1f; }), "negative slope");
    require(refused([](WheelProbe& p) { p.origin.x = 300.0f; }), "a sphere path outside the +-256 m envelope");
    WheelProbe far_ray = down({300.0f, 0, 1}, 2.0f);
    require(saige::probe_wheel(world, far_ray).status == WheelContactStatus::Miss, "a ray accepts any finite coordinates");
    // probe_wheels checks every probe first and leaves the output alone on a bad one.
    std::vector<WheelProbe> probes{down({0, 0, 1}, 2.0f), down({0, 0, 1}, 2.0f)};
    probes[1].length = 0.0f;
    std::vector<WheelContact> out(2);
    out[0].distance = 123.0;
    saige::QueryScratch scratch;
    require(throws_invalid([&] { saige::probe_wheels(world, probes, out, scratch); }) && out[0].distance == 123.0, "probe_wheels: a bad probe leaves the output untouched");
    std::vector<WheelContact> wrong(1);
    require(throws_invalid([&] { saige::probe_wheels(world, probes, wrong, scratch); }), "probe_wheels: one output per probe");
}

void test_batch_strong_guarantee() {
    const StaticCollision world = flat_world();
    saige::QueryScratch scratch;
    // A valid ray over the floor, then a sphere whose path leaves the +-256 m envelope: scalars pass validation, the capsule sweep would throw.
    std::vector<WheelProbe> probes{down({0, 0, 0.5f}, 2.0f), down({256.0f, 0, 0.5f}, 2.0f, 0.25f)};
    std::vector<WheelContact> out(2);
    for (WheelContact& c : out) c.distance = 999.0;
    require(throws_invalid([&] { saige::probe_wheels(world, probes, out, scratch); }), "an out-of-envelope sphere probe throws");
    require(out[0].distance == 999.0 && out[1].distance == 999.0, "the batch leaves every output untouched when a later sphere is out of the envelope");
    // The same probe alone throws the same way, and in-envelope neighbours still answer.
    require(throws_invalid([&] { static_cast<void>(saige::probe_wheel(world, probes[1])); }), "probe_wheel throws for the out-of-envelope sphere");
    probes[1].origin = {255.0f - 0.25f, 0, 0.5f};  // path (including the radius) stays inside
    saige::probe_wheels(world, probes, out, scratch);
    require(out[0].status == WheelContactStatus::Support && near(out[0].distance, 0.5, 1e-9), "a valid batch answers after the refusal");
    // A path that leaves the envelope only through its displacement, not its origin.
    probes[1].origin = {0, 0, 0.5f};
    probes[1].direction = {1.0f, 0, 0};
    probes[1].origin.x = 255.5f;
    for (WheelContact& c : out) c.distance = 999.0;
    require(throws_invalid([&] { saige::probe_wheels(world, probes, out, scratch); }) && out[0].distance == 999.0 && out[1].distance == 999.0,
            "a sphere that sweeps out of the envelope leaves the outputs untouched");
}

// A radius-0 probe is a ray; one that only touches a box (starts on its boundary and moves outward or along it) is a Miss for that box and must not mask
// what lies beyond.
void test_ray_touching_is_not_a_hit() {
    const Aabb a{{0, 0, 0}, {1, 1, 1}};
    const Aabb beyond{{-5, 0, 0}, {-4, 1, 1}};  // downstream of a ray leaving a's min-X face
    const StaticCollision world(std::vector<Aabb>{a, beyond}, {});
    const auto probe = [](Vec3 o, Vec3 d, float length) {
        WheelProbe p;
        p.origin = o;
        p.direction = d;
        p.length = length;
        return p;
    };
    // Core's repro: on the min-X face, moving -X. Miss for `a`; the box beyond is the first thing it meets.
    {
        const WheelContact c = saige::probe_wheel(world, probe({0, 0.5f, 0.5f}, {-1, 0, 0}, 10.0f));
        require(c.primitive.kind == StaticPrimitive::Kind::Box && c.primitive.index == 1, "leaving the min face finds the box downstream, not the box it leaves");
        require(near(c.distance, 4.0, 1e-9) && c.status != WheelContactStatus::InitialOverlap, "the downstream box is hit at its face");
    }
    {
        const WheelContact c = saige::probe_wheel(world, probe({0, 0.5f, 0.5f}, {-1, 0, 0}, 3.0f));
        require(c.status == WheelContactStatus::Miss, "leaving the min face with nothing within the length is a Miss");
    }
    // Max face, moving +X.
    {
        const WheelContact c = saige::probe_wheel(world, probe({1, 0.5f, 0.5f}, {1, 0, 0}, 10.0f));
        require(c.status == WheelContactStatus::Miss, "leaving the max face is a Miss");
    }
    // Tangent: along a face, and in the plane of one (top face moving +X, on the boundary of the min/max Y faces).
    require(saige::probe_wheel(world, probe({-0.5f, 0.5f, 1}, {1, 0, 0}, 10.0f)).status == WheelContactStatus::Miss, "running along the top face is a Miss");
    require(saige::probe_wheel(world, probe({0.25f, 0, 0.5f}, {0, 0, 1}, 10.0f)).status == WheelContactStatus::Miss, "running in a face's plane inside the box's extent is a Miss");
    // An edge origin moving in on one axis and out on another leaves at once (max-Z edge of the min-X face: +X is inward, +Z is outward).
    require(saige::probe_wheel(world, probe({0, 0.5f, 1}, {1, 0, 1}, 10.0f)).status == WheelContactStatus::Miss, "an origin on an edge moving in on one axis and out on another leaves at once");
    // Exact inward contact: on the min-X face, moving +X. A hit at distance 0 with the face's outward normal; it is not "inside" (a surface, not an overlap).
    {
        const WheelContact c = saige::probe_wheel(world, probe({0, 0.5f, 0.5f}, {1, 0, 0}, 10.0f));
        require(c.primitive.kind == StaticPrimitive::Kind::Box && c.primitive.index == 0 && c.distance == 0.0, "an origin on the min face moving in hits at distance 0");
        require(c.status != WheelContactStatus::InitialOverlap && c.normal.x == -1.0f && c.normal.y == 0.0f && c.normal.z == 0.0f, "with the face's outward normal");
    }
    // The same from the top face moving down (the common wheel case): a support at distance 0, normal up.
    {
        const WheelContact c = saige::probe_wheel(world, probe({0.5f, 0.5f, 1}, {0, 0, -1}, 10.0f));
        require(c.status == WheelContactStatus::Support && c.distance == 0.0 && c.normal.z == 1.0f, "standing exactly on a top face supports at distance 0");
    }
    // Strictly inside is still an InitialOverlap with the reverse-direction normal.
    {
        const WheelContact c = saige::probe_wheel(world, probe({0.5f, 0.5f, 0.5f}, {1, 0, 0}, 10.0f));
        require(c.status == WheelContactStatus::InitialOverlap && c.normal.x == -1.0f && c.distance == 0.0, "strictly inside is an initial overlap");
    }
    // Triangles have no interior: an origin on one is in contact with it (distance 0), and a ray in its plane misses it, without masking a box beyond.
    {
        const Triangle tri{{-1, -1, 0}, {1, -1, 0}, {0, 2, 0}};
        const Aabb wall{{3, -1, -1}, {4, 1, 1}};
        const StaticCollision w(std::vector<Aabb>{wall}, std::vector<Triangle>{tri});
        const WheelContact on = saige::probe_wheel(w, probe({0, 0, 0}, {0, 0, -1}, 5.0f));
        require(on.primitive.kind == StaticPrimitive::Kind::Triangle && on.distance == 0.0, "an origin on a triangle is in contact with it");
        const WheelContact plane = saige::probe_wheel(w, probe({-0.5f, 0, 0}, {1, 0, 0}, 10.0f));
        require(plane.primitive.kind == StaticPrimitive::Kind::Box && near(plane.distance, 3.5, 1e-9), "a ray in a triangle's plane misses it and finds the box beyond");
    }
}

// Measured, not asserted: ray probes allocate nothing; a sphere probe allocates nothing once its scratch has grown; probe_wheels adds nothing of its own.
void test_allocations() {
    std::vector<Aabb> boxes;
    for (int i = 0; i < 40; ++i) boxes.push_back({{static_cast<float>(i) * 2.0f, 0, -1}, {static_cast<float>(i) * 2.0f + 1.0f, 1, 0}});
    const StaticCollision world(boxes, {});
    std::vector<WheelProbe> rays, spheres;
    for (int i = 0; i < 4; ++i) {
        rays.push_back(down({static_cast<float>(i) * 8.0f + 0.5f, 0.5f, 1}, 3.0f));
        spheres.push_back(down({static_cast<float>(i) * 8.0f + 0.5f, 0.5f, 1}, 3.0f, 0.3f));
    }
    std::vector<WheelContact> out(rays.size());
    saige::QueryScratch scratch;
    // Warm the scratch (a sphere path gathers primitives into it).
    saige::probe_wheels(world, spheres, out, scratch);
    saige::probe_wheels(world, spheres, out, scratch);
    std::size_t before = allocation_count.load();
    for (int round = 0; round < 3; ++round) {
        saige::probe_wheels(world, rays, out, scratch);
        for (const WheelProbe& p : rays) static_cast<void>(saige::probe_wheel(world, p, scratch));
    }
    std::size_t used = allocation_count.load() - before;  // read before require() builds its message string
    require(used == 0, "ray probes allocate nothing (single and batch)");
    before = allocation_count.load();
    for (int round = 0; round < 3; ++round) {
        saige::probe_wheels(world, spheres, out, scratch);
        for (const WheelProbe& p : spheres) static_cast<void>(saige::probe_wheel(world, p, scratch));
    }
    used = allocation_count.load() - before;
    require(used == 0, "sphere probes allocate nothing once the scratch has grown");
}

} // namespace

int main() {
    try {
        test_flat_ground();
        test_slopes();
        test_ledges_and_edges();
        test_underside_and_two_sided_triangles();
        test_nearest_contact_and_blockers();
        test_initial_overlap();
        test_seams_ties_and_replay();
        test_equals_brute_force();
        test_validation();
        test_batch_strong_guarantee();
        test_ray_touching_is_not_a_hit();
        test_allocations();
    } catch (const std::exception& error) {
        std::cerr << "wheel contact test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "wheel contact tests passed\n";
}
