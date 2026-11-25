// The compatibility gates of the dynamic rigid-body backend (include/saige/dynamics.hpp): static collision parity with StaticCollision, material behavior against the SAIGE table, stacking, constraints, continuous
// collision detection, deterministic replay, origin migration, and the first dynamic interactions with the SAIGE character-sized capsule and the SAIGE vehicle. Exit 77 (skipped) when no backend is compiled in.
#include "saige/dynamics.hpp"
#include "saige/vehicle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

constexpr double g = 9.81;

StaticCollision floor_only(float half = 200.0f) {
    std::vector<Aabb> boxes{{{-half, -half, -2.0f}, {half, half, 0.0f}}};
    return StaticCollision(boxes, {});
}

std::unique_ptr<DynamicsWorld> make(const StaticCollision& c, const SurfaceMap* s = nullptr, DynamicsConfig cfg = {}) {
    auto w = make_dynamics_world(DynamicsBackend::Jolt, cfg);
    require(w != nullptr, "the Jolt backend is compiled in");
    w->set_static_geometry(c, s);
    return w;
}

void run(DynamicsWorld& w, double seconds, double dt = 1.0 / 60.0) {
    const int n = static_cast<int>(std::lround(seconds / dt));
    for (int i = 0; i < n; ++i) w.step(dt);
}

DBodyDesc box_at(double x, double y, double z, double hx = 0.5, double hy = 0.5, double hz = 0.5) {
    DBodyDesc d;
    d.shape = DShapeKind::Box;
    d.half_extents = {hx, hy, hz};
    d.position = {x, y, z};
    d.mass = 20.0;
    return d;
}

void test_api() {
    require(!available_dynamics_backends().empty(), "a backend is available");
    auto w = make(floor_only());
    const DynamicsInfo i = w->info();
    std::printf("backend %s %s, %s precision, %s, continuous collision %d, determinism across thread counts: %s\n", i.backend.c_str(), i.version.c_str(), i.double_precision ? "double" : "single", i.multithreaded ? "multithreaded" : "single thread",
                i.continuous_collision ? 1 : 0, i.deterministic_across_threads ? "promised" : "not promised");
    require(i.backend == "jolt" && i.version == "5.7.0", "the pinned Jolt release (5.7.0)");
    const BodyId b = w->add_body(box_at(0, 0, 3));
    require(w->body_count() == 1 && b.valid(), "a body was added");
    w->remove_body(b);
    bool threw = false;
    try {
        (void)w->state(b);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw && w->body_count() == 0, "a removed body's id is stale and refused");
    const BodyId c = w->add_body(box_at(0, 0, 3));
    require(c.index == b.index && c.generation != b.generation, "its slot is reused with a new generation");
    const auto rejects = [&](DBodyDesc d) {
        try {
            (void)w->add_body(d);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    DBodyDesc bad = box_at(0, 0, 3);
    bad.position.x = std::nan("");
    require(rejects(bad), "a NaN position is rejected");
    bad = box_at(0, 0, 3, 0.0, 0.5, 0.5);
    require(rejects(bad), "a zero-size box is rejected");
    bad = box_at(0, 0, 3);
    bad.mass = -1.0;
    require(rejects(bad), "a negative mass is rejected");
    threw = false;
    try {
        w->step(0.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a zero step is rejected");
    threw = false;
    try {
        DynamicsConfig cfg;
        cfg.max_bodies = 2;
        (void)make_dynamics_world(DynamicsBackend::Jolt, cfg);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "an invalid configuration is rejected");
}

void test_static_parity() {
    // A world of boxes and a ramp (triangles) with two surfaces; 400 rays from the same random origins through the StaticCollision and through the backend must agree on whether and where they hit.
    std::vector<Aabb> boxes{{{-50, -50, -2}, {50, 50, 0}}, {{5, -3, 0}, {8, 3, 2}}, {{-9, 4, 0}, {-6, 9, 3.5f}}, {{12, -8, 0}, {13, 8, 5}}};
    std::vector<Triangle> tris{{{-5, -3, 0}, {-5, 3, 0}, {-1, -3, 1.5f}}, {{-5, 3, 0}, {-1, 3, 1.5f}, {-1, -3, 1.5f}}};
    const StaticCollision c(boxes, tris);
    SurfaceMap surfaces(c);
    surfaces.set({StaticPrimitive::Kind::Box, 1}, Surface::Gravel);
    auto w = make(c, &surfaces);
    std::uint32_t s = 99u;
    const auto rnd = [&] {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>(s >> 8) / 16777216.0;
    };
    int both = 0, mismatch = 0;
    double worst = 0.0, worst_normal = 0.0;
    for (int i = 0; i < 400; ++i) {
        const Vec3 from{static_cast<float>(-20 + 40 * rnd()), static_cast<float>(-15 + 30 * rnd()), static_cast<float>(1 + 8 * rnd())};
        const Vec3 to{static_cast<float>(-20 + 40 * rnd()), static_cast<float>(-15 + 30 * rnd()), static_cast<float>(-1 - 2 * rnd())};
        const Vec3 d{to.x - from.x, to.y - from.y, to.z - from.z};
        const double len = std::sqrt(static_cast<double>(d.x) * d.x + static_cast<double>(d.y) * d.y + static_cast<double>(d.z) * d.z);
        const auto ref = c.raycast(from, d, static_cast<float>(len));
        const DRayHit hit = w->cast_ray({from.x, from.y, from.z}, {to.x, to.y, to.z});
        if (ref.has_value() != hit.hit) {
            ++mismatch;
            continue;
        }
        if (!ref) continue;
        ++both;
        worst = std::max(worst, std::fabs(ref->distance / len - hit.fraction));
        const double nd = std::fabs(static_cast<double>(ref->normal.x) - hit.normal.x) + std::fabs(static_cast<double>(ref->normal.y) - hit.normal.y) + std::fabs(static_cast<double>(ref->normal.z) - hit.normal.z);
        if (ref->started_inside) continue;  // from inside a solid box StaticCollision reports a contact at distance 0 whose normal faces the origin; the backend reports a face: not comparable
        if (nd > 1e-2) std::printf("    normal differs: from (%.2f %.2f %.2f) kind %d inside %d: saige (%.2f %.2f %.2f) backend (%.2f %.2f %.2f) at fraction %.3f\n", from.x, from.y, from.z, static_cast<int>(ref->primitive.kind), ref->started_inside ? 1 : 0, ref->normal.x, ref->normal.y, ref->normal.z, hit.normal.x, hit.normal.y, hit.normal.z, hit.fraction);
        worst_normal = std::max(worst_normal, nd);
    }
    std::printf("static parity: %d rays hit both, %d disagreed on hit or miss, largest fraction difference %.2e, largest normal difference %.2e\n", both, mismatch, worst, worst_normal);
    require(both > 150, "control: many rays hit something");
    require(mismatch <= 2, "the backend and StaticCollision agree on hit or miss for the rays (a handful of grazing ones may differ)");
    require(worst < 1e-3, "and on where, to a thousandth of the ray");
    require(worst_normal < 1e-2, "and on the surface normal");
    // Bodies come to rest at the heights the geometry says: a box on the floor, on the box top, and on the ramp's plateau-free slope not at all (it slides): here the floor and the box top.
    for (const auto& [x, y, top] : {std::tuple<double, double, double>{0.0, 0.0, 0.0}, {6.5, 0.0, 2.0}, {-7.5, 6.5, 3.5}, {12.5, 0.0, 5.0}}) {
        auto w2 = make(c, &surfaces);
        const BodyId b = w2->add_body(box_at(x, y, top + 1.5, 0.4, 0.4, 0.3));
        const BodyId sp = w2->add_body([&] {
            DBodyDesc d;
            d.shape = DShapeKind::Sphere;
            d.half_extents = {0.25, 0, 0};
            d.position = {x - 0.1, y + 1.0 > 2.9 && x > 5.0 && x < 9.0 ? y : y, top + 3.0};
            d.mass = 5.0;
            return d;
        }());
        run(*w2, 4.0);
        const DBodyState st = w2->state(b);
        std::printf("  box above (%.1f, %.1f) top %.1f rests at %.3f (expected %.3f)\n", x, y, top, st.position.z, top + 0.3);
        require(std::fabs(st.position.z - (top + 0.3)) < 0.03, "a box rests on the surface it was dropped on, to the contact slop");
        (void)sp;
    }
}

void test_materials() {
    // Sliding down a 20 degree ramp: a = g (sin t - mu cos t) with mu the geometric mean of the body's and the surface's friction (SAIGE's rule), for three surfaces of the table. The ramp is a pair of triangles.
    const double theta = 20.0 * 3.14159265358979 / 180.0;
    for (const Surface surface : {Surface::Asphalt, Surface::Gravel, Surface::Ice}) {
        const float len = 30.0f, width = 6.0f;
        const float run_x = len * static_cast<float>(std::cos(theta)), rise = len * static_cast<float>(std::sin(theta));
        std::vector<Aabb> boxes{{{-100, -100, -rise - 5}, {100, 100, -rise - 1}}};
        std::vector<Triangle> tris{{{0, -width, rise}, {0, width, rise}, {run_x, -width, 0}}, {{0, width, rise}, {run_x, width, 0}, {run_x, -width, 0}}};
        const StaticCollision c(boxes, tris);
        SurfaceMap surfaces(c);
        for (std::size_t i = 0; i < c.triangle_count(); ++i) surfaces.set({StaticPrimitive::Kind::Triangle, i}, surface);
        auto w = make(c, &surfaces);
        // Place the box on the slope, aligned with it: rotation about +Y by +theta tilts +X down (SAIGE: +Z up, the slope descends toward +X).
        const double mid = 6.0;
        const double zs = rise - mid * std::tan(theta);
        DBodyDesc d = box_at(mid + 0.0, 0.0, zs, 0.4, 0.4, 0.2);
        d.surface = Surface::Concrete;
        const double h = 0.2;  // half height: lift the center along the normal
        d.position = {mid + h * std::sin(theta), 0.0, zs + h * std::cos(theta) + 0.002};
        d.rotation = {0.0, std::sin(theta / 2.0), 0.0, std::cos(theta / 2.0)};
        const BodyId b = w->add_body(d);
        run(*w, 0.3);  // settle into contact
        const DBodyState s0 = w->state(b);
        run(*w, 1.0);
        const DBodyState s1 = w->state(b);
        const double v0 = std::hypot(s0.linear_velocity.x, s0.linear_velocity.z), v1 = std::hypot(s1.linear_velocity.x, s1.linear_velocity.z);
        const double measured = (v1 - v0) / 1.0;
        const double mu = std::sqrt(static_cast<double>(physics_material(Surface::Concrete).friction) * static_cast<double>(physics_material(surface).friction));
        const double expected = std::max(0.0, g * (std::sin(theta) - mu * std::cos(theta)));
        std::printf("  slope, %s: acceleration %.3f m/s^2 (g (sin - mu cos) = %.3f with mu %.3f)\n", physics_material(surface).name, measured, expected, mu);
        require(std::fabs(measured - expected) < 0.06 * std::max(expected, 0.5) + 0.05, "the sliding acceleration follows the friction the SAIGE material rule gives");
    }
    // Restitution: the larger of the two. A sphere dropped 2 m onto a floor of each: the rebound apex over the drop height is e squared.
    for (const Surface surface : {Surface::Concrete, Surface::Rubber}) {
        SurfaceMap* none = nullptr;
        (void)none;
        const StaticCollision c = floor_only();
        SurfaceMap surfaces(c);
        surfaces.set({StaticPrimitive::Kind::Box, 0}, surface);
        auto w = make(c, &surfaces);
        DBodyDesc d;
        d.shape = DShapeKind::Sphere;
        d.half_extents = {0.2, 0, 0};
        d.position = {0, 0, 2.2};
        d.mass = 2.0;
        d.surface = Surface::Default;  // restitution 0.2
        const BodyId b = w->add_body(d);
        double peak_after = 0.0;
        bool bounced = false;
        double last_vz = 0.0;
        for (int i = 0; i < 240 * 3; ++i) {
            w->step(1.0 / 240.0);  // fine steps so the apex is resolved
            const DBodyState st = w->state(b);
            if (!bounced && last_vz < -1.0 && st.linear_velocity.z > 0.0) bounced = true;
            if (bounced) peak_after = std::max(peak_after, st.position.z - 0.2);
            last_vz = st.linear_velocity.z;
        }
        const double e = std::max(static_cast<double>(physics_material(Surface::Default).restitution), static_cast<double>(physics_material(surface).restitution));
        const double ratio = peak_after / 2.0;
        std::printf("  bounce on %s: apex %.3f of a 2 m drop (e^2 = %.3f with e = max(0.2, %.2f))\n", physics_material(surface).name, ratio, e * e, static_cast<double>(physics_material(surface).restitution));
        require(std::fabs(ratio - e * e) < 0.08 * e * e + 0.01, "the rebound is the larger restitution of the pair, squared, of the drop height");
    }
    // An explicit friction overrides the table.
    {
        const StaticCollision c = floor_only();
        auto w = make(c);
        DBodyDesc d = box_at(0, 0, 0.5);
        d.friction = 0.0f;
        d.linear_velocity = {5, 0, 0};
        const BodyId slick = w->add_body(d);
        d.position = {0, 5, 0.5};
        d.friction = 1.0f;
        const BodyId grippy = w->add_body(d);
        run(*w, 1.0);
        require(w->state(slick).linear_velocity.x > 4.9 && w->state(grippy).linear_velocity.x < 3.5, "an explicit friction of 0 slides on and of 1 brakes: the override works");
    }
}

void test_stacking() {
    const StaticCollision c = floor_only();
    for (const int height : {5, 10}) {
        auto w = make(c);
        std::vector<BodyId> ids;
        for (int i = 0; i < height; ++i) ids.push_back(w->add_body(box_at(0, 0, 0.5 + static_cast<double>(i) * 1.0 + 0.001 * i, 0.5, 0.5, 0.5)));
        run(*w, 10.0);
        double drift = 0.0, speed = 0.0;
        for (int i = 0; i < height; ++i) {
            const DBodyState s = w->state(ids[static_cast<std::size_t>(i)]);
            drift = std::max(drift, std::hypot(s.position.x, s.position.y));
            speed = std::max(speed, std::hypot(s.linear_velocity.x, std::hypot(s.linear_velocity.y, s.linear_velocity.z)));
        }
        const DBodyState top = w->state(ids.back());
        std::printf("  stack of %d boxes after 10 s: top at %.3f (expected %.3f), sideways drift %.4f m, fastest %.4f m/s, top asleep %d\n", height, top.position.z, 0.5 + height - 1.0, drift, speed, top.active ? 0 : 1);
        require(std::fabs(top.position.z - (0.5 + height - 1.0)) < 0.05 * height * 0.2 + 0.05, "the stack stands at its height (less a few centimeters of settling)");
        require(drift < 0.05 && speed < 0.05, "and does not drift or move");
    }
    // A pyramid of 15 boxes.
    auto w = make(c);
    std::vector<BodyId> ids;
    for (int row = 0; row < 5; ++row) {
        for (int k = 0; k < 5 - row; ++k) ids.push_back(w->add_body(box_at((k - (5 - row - 1) * 0.5) * 1.02, 0.0, 0.5 + row * 1.0 + 0.002, 0.5, 0.5, 0.5)));
    }
    run(*w, 8.0);
    double top_z = w->state(ids.back()).position.z;
    std::printf("  pyramid of 15 after 8 s: apex at %.3f (expected 4.5)\n", top_z);
    require(std::fabs(top_z - 4.5) < 0.15, "a pyramid of 15 boxes stays a pyramid");
}

DVec rotate_x_axis(const DQuat& q) {  // the body's local +X in the world
    return {1 - 2 * (q.y * q.y + q.z * q.z), 2 * (q.x * q.y + q.z * q.w), 2 * (q.x * q.z - q.y * q.w)};
}

void test_constraints() {
    const StaticCollision c = floor_only();
    // A hinge: a bar hanging from a world pivot about +Y with limits [-0.4, 3.0] rad: released horizontally it swings down and past the limit it must stop.
    {
        auto w = make(c);
        DBodyDesc d = box_at(1.0, 0.0, 6.0, 1.0, 0.1, 0.1);  // a bar 2 m long, one end at the pivot (0, 0, 6)
        d.mass = 10.0;
        const BodyId bar = w->add_body(d);
        DConstraintDesc h;
        h.kind = DConstraintKind::Hinge;
        h.a = bar;
        h.pivot = {0, 0, 6};
        h.axis = {0, 1, 0};
        h.min_angle = -0.4;
        h.max_angle = 3.0;
        w->add_constraint(h);
        double lowest = 1e9, pivot_error = 0.0;
        for (int i = 0; i < 60 * 6; ++i) {
            w->step(1.0 / 120.0);
            const DBodyState s = w->state(bar);
            const DVec ax = rotate_x_axis(s.rotation);
            const double end_x = s.position.x - ax.x, end_z = s.position.z - ax.z;  // the end that began at the pivot
            pivot_error = std::max(pivot_error, std::hypot(end_x, end_z - 6.0));
            lowest = std::min(lowest, s.position.z);
        }
        std::printf("  hinge: pivot error %.4f m, the bar's center reached down to %.2f (pendulum length 1 m below 6: 5.0)\n", pivot_error, lowest);
        require(pivot_error < 0.02, "the hinge keeps its pivot");
        require(lowest > 4.95 && lowest < 5.06, "the bar swings to the bottom, no further (a pendulum of length 1 m)");
    }
    // Hinge limit: a bar released from horizontal with a max angle of 0.5 rad cannot fall further than that.
    {
        auto w = make(c);
        DBodyDesc d = box_at(1.0, 0.0, 6.0, 1.0, 0.1, 0.1);
        d.mass = 10.0;
        const BodyId bar = w->add_body(d);
        DConstraintDesc h;
        h.kind = DConstraintKind::Hinge;
        h.a = bar;
        h.pivot = {0, 0, 6};
        h.axis = {0, 1, 0};
        h.min_angle = -0.1;
        h.max_angle = 0.5;
        w->add_constraint(h);
        run(*w, 3.0, 1.0 / 120.0);
        const DBodyState s = w->state(bar);
        const double drop = std::atan2(6.0 - s.position.z, s.position.x);  // the angle below horizontal
        std::printf("  hinge limit: the bar hangs %.3f rad below horizontal (limit 0.5)\n", drop);
        require(drop > 0.4 && drop < 0.56, "a hinge's limit holds the bar at about its limit angle");
    }
    // Swing-twist: a bar hanging by one end with a cone of 0.5 rad is struck sideways; it must stay in the cone. A moderate blow (10 m/s at the bar's center) is a requirement; a violent one (40 m/s) is measured and reported
    // (the constraint's position correction needs several steps to bring a bar back that a single step carried far past the limit).
    for (const double blow : {100.0, 400.0}) {
        auto w = make(c);
        DBodyDesc d = box_at(0.0, 0.0, 5.0, 0.1, 0.1, 1.0);  // hanging down from (0,0,6)
        d.mass = 10.0;
        const BodyId bar = w->add_body(d);
        DConstraintDesc s;
        s.kind = DConstraintKind::SwingTwist;
        s.a = bar;
        s.pivot = {0, 0, 6};
        s.axis = {0, 0, -1};
        s.swing_half_angle = 0.5;
        s.min_angle = -0.3;
        s.max_angle = 0.3;
        w->add_constraint(s);
        double worst = 0.0;
        for (int i = 0; i < 60 * 4; ++i) {
            if (i == 10) w->add_impulse(bar, {blow, 0, 0}, {0, 0, 5.0});
            w->step(1.0 / 120.0);
            const DBodyState st = w->state(bar);
            worst = std::max(worst, std::atan2(std::hypot(st.position.x, st.position.y), 6.0 - st.position.z));
        }
        std::printf("  swing-twist, a blow of %.0f N s on 10 kg: the bar swung out to %.3f rad (cone half angle 0.5)\n", blow, worst);
        if (blow < 200.0) require(worst > 0.35 && worst < 0.6, "it swings out to the cone and, at most, a few hundredths of a radian further");
        else require(worst < 0.9, "even a violent blow is held within a bounded overshoot");
    }
    // Fixed and distance: two boxes welded stay welded under a load; a rope of 2 m stops a falling weight.
    {
        auto w = make(c);
        const BodyId a = w->add_body(box_at(0, 0, 4.0));
        const BodyId b = w->add_body(box_at(1.0, 0, 4.0));
        DConstraintDesc f;
        f.kind = DConstraintKind::Fixed;
        f.a = a;
        f.b = b;
        f.pivot = {0.5, 0, 4.0};
        w->add_constraint(f);
        DConstraintDesc anchor = f;
        anchor.b = BodyId{};
        anchor.a = a;
        w->add_constraint(anchor);  // a is fixed to the world as well
        run(*w, 3.0, 1.0 / 120.0);
        const DBodyState sa = w->state(a), sb = w->state(b);
        std::printf("  fixed: separation %.4f m (1.0 at creation), sag %.4f m\n", sb.position.x - sa.position.x, 4.0 - sb.position.z);
        require(std::fabs(sb.position.x - sa.position.x - 1.0) < 0.02 && 4.0 - sb.position.z < 0.05, "welded boxes keep their relative pose and a fixed-to-world pair does not fall");
        auto w2 = make(c);
        DBodyDesc d;
        d.shape = DShapeKind::Sphere;
        d.half_extents = {0.2, 0, 0};
        d.position = {0, 0, 9.0};
        d.mass = 5.0;
        const BodyId weight = w2->add_body(d);
        DConstraintDesc r;
        r.kind = DConstraintKind::Distance;
        r.a = weight;
        r.pivot = {0, 0, 9.0};
        r.use_pivot_b = true;
        r.pivot_b = {0, 0, 10.0};
        r.min_distance = 0.0;
        r.max_distance = 2.0;
        w2->add_constraint(r);
        run(*w2, 2.0, 1.0 / 120.0);
        const double len = std::hypot(w2->state(weight).position.x, w2->state(weight).position.z - 10.0);
        std::printf("  distance: the weight hangs %.3f m from its anchor (rope 2 m; it began 1 m away)\n", len);
        require(len > 1.9 && len < 2.08, "a rope of 2 m holds the weight at about 2 m");
    }
    // A chain of ten links hanging from the world: stable, finite, its length close to the sum of the links.
    {
        auto w = make(c);
        std::vector<BodyId> links;
        for (int i = 0; i < 10; ++i) {
            DBodyDesc d = box_at(0.0, 0.0, 9.5 - i * 0.5, 0.05, 0.05, 0.25);
            d.mass = 1.0;
            links.push_back(w->add_body(d));
        }
        for (int i = 0; i < 10; ++i) {
            DConstraintDesc h;
            h.kind = DConstraintKind::SwingTwist;
            h.a = i == 0 ? links[0] : links[static_cast<std::size_t>(i - 1)];
            h.b = i == 0 ? BodyId{} : links[static_cast<std::size_t>(i)];
            h.pivot = {0, 0, 9.75 - i * 0.5};
            h.axis = {0, 0, -1};
            h.swing_half_angle = 1.0;
            h.min_angle = -0.5;
            h.max_angle = 0.5;
            w->add_constraint(h);
        }
        w->add_impulse(links.back(), {30, 0, 0}, w->state(links.back()).position);
        run(*w, 6.0, 1.0 / 120.0);
        const DVec top = w->state(links.front()).position, end = w->state(links.back()).position;
        const double span = std::sqrt((top.x - end.x) * (top.x - end.x) + (top.z - end.z) * (top.z - end.z));
        std::printf("  chain of ten links: top to end %.3f m (4.5 when straight)\n", span);
        require(std::isfinite(span) && span > 4.2 && span < 4.6, "a chain of ten links is stable and as long as its links");
    }
}

void test_ccd() {
    // A thin wall (5 cm) and a fast ball (150 m/s): without continuous collision the ball passes through between steps; with it, it is stopped.
    std::vector<Aabb> boxes{{{-100, -100, -2}, {100, 100, 0}}, {{10.0f, -5, 0}, {10.05f, 5, 4}}};
    const StaticCollision c(boxes, {});
    for (const bool continuous : {false, true}) {
        auto w = make(c);
        DBodyDesc d;
        d.shape = DShapeKind::Sphere;
        d.half_extents = {0.05, 0, 0};
        d.position = {0, 0, 1.0};
        d.mass = 0.5;
        d.continuous = continuous;
        d.linear_velocity = {143, 0, 0};  // 2.38 m per step: not a divisor of the 10 m to the wall, so the discrete step cannot land in it
        const BodyId b = w->add_body(d);
        double xmax = 0.0;
        for (int i = 0; i < 60; ++i) {
            w->step(1.0 / 60.0);
            xmax = std::max(xmax, w->state(b).position.x);
        }
        std::printf("  143 m/s ball against a 5 cm wall, continuous %d: furthest x %.2f (wall at 10)\n", continuous ? 1 : 0, xmax);
        if (continuous) require(xmax < 10.0, "continuous collision detection stops the ball at the wall");
        else require(xmax > 10.5, "control: without it the ball tunnels through");
    }
}

std::vector<DBodyState> pile(unsigned threads, int steps, int count = 80) {
    DynamicsConfig cfg;
    cfg.threads = threads;
    auto w = make(floor_only(), nullptr, cfg);
    std::vector<BodyId> ids;
    std::uint32_t s = 7u;
    const auto rnd = [&] {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>(s >> 8) / 16777216.0;
    };
    for (int i = 0; i < count; ++i) {
        DBodyDesc d = box_at(-3 + 6 * rnd(), -3 + 6 * rnd(), 1.0 + i * 0.7, 0.3 + 0.2 * rnd(), 0.3 + 0.2 * rnd(), 0.3 + 0.2 * rnd());
        d.rotation = {0.0, 0.0, std::sin(rnd()), std::cos(rnd())};
        d.mass = 5.0 + 20.0 * rnd();
        ids.push_back(w->add_body(d));
    }
    for (int i = 0; i < steps; ++i) w->step(1.0 / 60.0);
    std::vector<DBodyState> out;
    for (const BodyId id : ids) out.push_back(w->state(id));
    return out;
}

bool identical(const std::vector<DBodyState>& a, const std::vector<DBodyState>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::memcmp(&a[i].position, &b[i].position, sizeof(DVec)) != 0 || std::memcmp(&a[i].rotation, &b[i].rotation, sizeof(DQuat)) != 0 || std::memcmp(&a[i].linear_velocity, &b[i].linear_velocity, sizeof(DVec)) != 0) return false;
    }
    return true;
}

void test_replay() {
    const auto a = pile(1, 150), b = pile(1, 150);
    require(identical(a, b), "two worlds built the same and stepped the same are bit-identical after 150 steps (80 boxes in a pile)");
    // Different input gives a different result (the comparison is not vacuous).
    const auto c = pile(1, 149);
    require(!identical(a, c), "control: one step fewer is a different state");
    // Thread counts: reported, and required only if the build promises it.
    const auto t4 = pile(4, 150);
    const bool same = identical(a, t4);
    auto w = make(floor_only());
    std::printf("  one thread against four: %s (the build %s a result independent of the thread count)\n", same ? "identical" : "different", w->info().deterministic_across_threads ? "promises" : "does not promise");
    if (w->info().deterministic_across_threads) require(same, "a build that promises determinism across thread counts delivers it");
}

void test_origin_shift() {
    // The same pile run in two frames: the second is shifted by (60, -35, 0) after 100 steps (the geometry set again in the new frame); the states agree after the shift is undone, to float precision.
    const DVec shift{60.0, -35.0, 0.0};
    const auto build = [&](bool shifted) {
        auto w = make(floor_only());
        std::vector<BodyId> ids;
        for (int i = 0; i < 12; ++i) ids.push_back(w->add_body(box_at(-2.0 + (i % 4), -1.0 + (i / 4), 1.0 + i * 0.6, 0.4, 0.4, 0.4)));
        for (int i = 0; i < 100; ++i) w->step(1.0 / 60.0);
        if (shifted) {
            w->shift_origin(shift);
            w->set_static_geometry(StaticCollision(std::vector<Aabb>{{{-200.0f + 60.0f, -200.0f - 35.0f, -2.0f}, {200.0f + 60.0f, 200.0f - 35.0f, 0.0f}}}, {}), nullptr);
        }
        for (int i = 0; i < 300; ++i) w->step(1.0 / 60.0);
        std::vector<DBodyState> out;
        for (const BodyId id : ids) out.push_back(w->state(id));
        return out;
    };
    const auto plain = build(false), moved = build(true);
    double worst = 0.0;
    for (std::size_t i = 0; i < plain.size(); ++i) worst = std::max(worst, std::fabs(plain[i].position.x - (moved[i].position.x - shift.x)) + std::fabs(plain[i].position.y - (moved[i].position.y - shift.y)) + std::fabs(plain[i].position.z - moved[i].position.z));
    std::printf("  origin shift of (60, -35) at step 100: largest position difference after the shift is undone %.2e m\n", worst);
    require(worst < 2e-3, "a world shifted by an origin migration continues as it would have (differences at float precision)");
}

void test_dynamic_interactions() {
    const StaticCollision c = floor_only();
    // A kinematic capsule, the size of a character (radius 0.3, 1.7 m), walks into a 20 kg crate at 3 m/s: the crate is carried at about the walking speed.
    {
        auto w = make(c);
        DBodyDesc cap;
        cap.shape = DShapeKind::Capsule;
        cap.half_extents = {0.3, 0.55, 0};
        cap.motion = DMotion::Kinematic;
        cap.position = {-3.0, 0.0, 0.85};
        cap.rotation = {std::sin(3.14159265358979 / 4.0), 0.0, 0.0, std::cos(3.14159265358979 / 4.0)};  // the capsule's axis is the body's Z: turn it upright (Jolt's capsule is along Y; the adapter keeps the body's Z axis for SAIGE)
        const BodyId person = w->add_body(cap);
        DBodyDesc crate = box_at(0.0, 0.0, 0.4, 0.4, 0.4, 0.4);
        crate.mass = 20.0;
        const BodyId box = w->add_body(crate);
        double x = -3.0, vmax = 0.0;
        for (int i = 0; i < 60 * 3; ++i) {
            x += 3.0 / 60.0;
            w->set_kinematic_target(person, {x, 0.0, 0.85}, cap.rotation, 1.0 / 60.0);
            w->step(1.0 / 60.0);
            vmax = std::max(vmax, w->state(box).linear_velocity.x);
        }
        std::printf("  a character-sized kinematic capsule walking at 3 m/s pushes a 20 kg crate to at most %.2f m/s; the crate is at x = %.2f\n", vmax, w->state(box).position.x);
        require(vmax > 2.5 && vmax < 4.5, "the crate is carried at about the walking speed (the kinematic body has no give)");
        require(w->state(box).position.x > 0.8, "and has been pushed along");
    }
    // The SAIGE vehicle (its own simulation, 1450 kg at 15 m/s) shadowed by a kinematic box drives into five crates: they are thrown ahead, momentum is plausible, and the vehicle (one-way coupling) is not slowed by them.
    {
        auto w = make(c);
        VehicleConfig vc;
        vc.powertrain.enabled = true;
        Vehicle car(vc, {-60, 0, 0.9f}, 0.0f);
        DBodyDesc proxy;
        proxy.shape = DShapeKind::Box;
        proxy.half_extents = {vc.half_extents.x, vc.half_extents.y, vc.half_extents.z};
        proxy.motion = DMotion::Kinematic;
        proxy.position = {-60, 0, 0.9};
        const BodyId chassis = w->add_body(proxy);
        std::vector<BodyId> crates;
        for (int i = 0; i < 5; ++i) crates.push_back(w->add_body(box_at(0.0, -2.0 + i, 0.4, 0.4, 0.4, 0.4)));
        QueryScratch scratch;
        VehicleInput in;
        double crate_momentum = 0.0, car_speed_at_hit = 0.0, crate_peak = 0.0;
        for (int i = 0; i < 60 * 14; ++i) {
            in.throttle = static_cast<float>(std::clamp((15.0 - car.state().forward_speed) * 0.6, -0.5, 1.0));
            car.step(in, 1.0f / 60.0f, c, scratch);
            const VehicleState& st = car.state();
            w->set_kinematic_target(chassis, {st.position.x, st.position.y, st.position.z}, {st.orientation.x, st.orientation.y, st.orientation.z, st.orientation.w}, 1.0 / 60.0);
            w->step(1.0 / 60.0);
            if (st.position.x + vc.half_extents.x > -0.5 && car_speed_at_hit == 0.0) car_speed_at_hit = st.forward_speed;
            for (const BodyId b : crates) crate_peak = std::max(crate_peak, w->state(b).linear_velocity.x);
        }
        double farthest = 0.0;
        for (const BodyId b : crates) {
            const DBodyState s = w->state(b);
            crate_momentum += 20.0 * s.linear_velocity.x;
            farthest = std::max(farthest, s.position.x);
        }
        std::printf("  the vehicle at %.1f m/s into five 20 kg crates: the fastest crate reaches %.1f m/s, they end up to x = %.1f m (the car carried %.0f kg m/s; one-way coupling: the car's own simulation is not slowed)\n", car_speed_at_hit, crate_peak, farthest, 1450.0 * car_speed_at_hit);
        require(car_speed_at_hit > 12.0, "control: the car arrived at speed");
        require(farthest > 8.0, "the crates were thrown ahead of the car's path");
        require(crate_peak > 0.9 * car_speed_at_hit && crate_peak < 2.2 * car_speed_at_hit, "no crate is thrown faster than an elastic hit by an immovable body allows (twice the car's speed), and it is at least carried along");
        (void)crate_momentum;
    }
}

} // namespace

int main(int argc, char** argv) {
    if (available_dynamics_backends().empty()) {
        std::cout << "no dynamics backend is compiled in (configure with -DSAIGE_PHYSICS_JOLT=ON): skipped\n";
        return 77;
    }
    struct Named {
        const char* name;
        void (*fn)();
    };
    const Named tests[] = {{"api", test_api},           {"parity", test_static_parity}, {"materials", test_materials},   {"stacking", test_stacking}, {"constraints", test_constraints},
                           {"ccd", test_ccd},           {"replay", test_replay},        {"origin", test_origin_shift},   {"interactions", test_dynamic_interactions}};
    int failures = 0;
    for (const Named& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cerr << "FAIL (" << t.name << "): " << e.what() << "\n";
            ++failures;
        }
    }
    if (failures == 0) std::cout << "dynamics tests passed\n";
    return failures == 0 ? 0 : 1;
}
