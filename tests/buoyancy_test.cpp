// Buoyancy on bodies of the dynamics world (include/saige/buoyancy.hpp): Archimedes' draft, a heavy body sinking at the drag balance, agreement with the water module's own FloatingBody in waves, a heeled box
// righting itself, and a drop into the water. Exit 77 (skipped) when no dynamics backend is compiled in.
#include "saige/buoyancy.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

constexpr double pi = 3.14159265358979323846;

std::unique_ptr<DynamicsWorld> make_world() {
    DynamicsConfig cfg;
    cfg.collision_steps = 2;
    auto w = make_dynamics_world(DynamicsBackend::Jolt, cfg);
    require(w != nullptr, "a backend");
    // No floor within reach: the sea is deep. A far floor keeps the world non-empty.
    w->set_static_geometry(StaticCollision(std::vector<Aabb>{{{-500, -500, -300}, {500, 500, -290}}}, {}), nullptr);
    return w;
}

struct Rig {
    std::unique_ptr<DynamicsWorld> world = make_world();
    BodyId body;
    BuoyantBox box;
    double t = 0.0;
    Rig(DVec half, double density, DVec position, DQuat rotation = {}) {
        DBodyDesc d;
        d.shape = DShapeKind::Box;
        d.half_extents = half;
        d.position = position;
        d.rotation = rotation;
        d.mass = density * 8.0 * half.x * half.y * half.z;
        body = world->add_body(d);
        box.half_extents = half;
        box.mass = d.mass;
    }
    BuoyancyResult step(const WaterSource& water, double dt = 1.0 / 120.0) {
        const BuoyancyResult r = apply_buoyancy(*world, body, box, water, t, dt);
        world->step(dt);
        t += dt;
        return r;
    }
};

double roll_of(const DQuat& q) { return 2.0 * std::atan2(q.x, q.w); }

void test_draft() {
    const WaterSurface calm{WaterConfig{}};
    for (const double density : {250.0, 600.0, 900.0}) {
        Rig r({0.5, 0.5, 0.25}, density, {0, 0, 0.3});
        BuoyancyResult last;
        for (int i = 0; i < 120 * 20; ++i) last = r.step(calm);
        const DBodyState s = r.world->state(r.body);
        const double draft = (density / 1000.0) * 0.5, expected = draft - 0.25;  // the bottom is 0.25 below the centre; the waterline is `draft` above the bottom
        std::printf("  density %.0f: settled centre at %.4f (Archimedes %.4f), submerged volume %.4f (%.4f), speed %.4f\n", density, s.position.z, -expected, last.submerged_volume, density / 1000.0 * 0.5, std::fabs(s.linear_velocity.z));
        require(std::fabs(s.position.z - (-expected)) < 0.01, "a floating box settles at Archimedes' draft, to a centimeter");
        require(std::fabs(last.submerged_volume - density / 1000.0 * 0.5) < 0.02, "its submerged volume is its mass over the water's density");
        require(std::fabs(s.linear_velocity.z) < 0.05, "and it is (nearly) at rest: the residual heave is under 5 cm/s (the cell grid makes the submerged volume step, so a small limit cycle remains)");
    }
}

void test_sinking() {
    const WaterSurface deep{WaterConfig{}};
    Rig a({0.3, 0.3, 0.3}, 1500.0, {0, 0, -0.2});
    FloatingBodyConfig fc;
    fc.half_extents = {0.3, 0.3, 0.3};
    fc.density = 1500.0;
    FloatingBody ref(fc, {0, 0, -0.2});
    double t = 0.0;
    for (int i = 0; i < 120 * 8; ++i) {
        a.step(deep);
        t = ref.step(1.0 / 120.0, deep, t);
    }
    const DBodyState s = a.world->state(a.body);
    std::printf("  a box of density 1500 after 8 s: depth %.2f m at %.3f m/s (the water module's own body: %.2f m at %.3f m/s)\n", s.position.z, s.linear_velocity.z, ref.state().position.z, ref.state().velocity.z);
    require(s.position.z < -5.0, "a heavy body sinks");
    require(std::fabs(s.linear_velocity.z - ref.state().velocity.z) < 0.1 * std::fabs(ref.state().velocity.z) + 0.02, "at the terminal speed the water module's body reaches (within 10%)");
    require(std::fabs(s.position.z - ref.state().position.z) < 0.1 * std::fabs(ref.state().position.z), "and to the same depth (within 10%)");
}

void test_waves() {
    WaterConfig wc;
    wc.waves = {{0.0, 80.0, 0.4, 0.0}};
    const WaterSurface sea(wc);
    FloatingBodyConfig fc;
    fc.half_extents = {0.6, 0.6, 0.3};
    fc.density = 500.0;
    FloatingBody ref(fc, {0, 0, 0});
    Rig r({0.6, 0.6, 0.3}, 500.0, {0, 0, 0});
    double t = 0.0, rmax = -1e9, rmin = 1e9, dmax = -1e9, dmin = 1e9, err = 0.0;
    int n = 0;
    for (int i = 0; i < 120 * 60; ++i) {
        r.step(sea);
        t = ref.step(1.0 / 120.0, sea, t);
        if (t > 30.0) {
            const double zr = r.world->state(r.body).position.z, zf = ref.state().position.z;
            rmax = std::max(rmax, zr);
            rmin = std::min(rmin, zr);
            dmax = std::max(dmax, zf);
            dmin = std::min(dmin, zf);
            err += (zr - zf) * (zr - zf);
            ++n;
        }
    }
    const double rms = std::sqrt(err / n);
    std::printf("  a buoy on an 80 m wave of amplitude 0.4: heave amplitude %.3f m on the dynamics body, %.3f m on the water module's; RMS height difference %.3f m\n", 0.5 * (rmax - rmin), 0.5 * (dmax - dmin), rms);
    require(std::fabs(0.5 * (rmax - rmin) / (0.5 * (dmax - dmin)) - 1.0) < 0.1, "the dynamics body heaves with the same amplitude as the water module's (within 10%)");
    require(rms < 0.08, "and follows it to within 8 cm RMS over 30 s");
    require(0.5 * (rmax - rmin) > 0.3, "control: it does heave");
}

void test_righting() {
    const WaterSurface calm{WaterConfig{}};
    const double angle = 25.0 * pi / 180.0;
    Rig r({1.0, 1.5, 0.2}, 500.0, {0, 0, 0}, {std::sin(angle / 2), 0, 0, std::cos(angle / 2)});
    double first = roll_of(r.world->state(r.body).rotation), late = 0.0;
    for (int i = 0; i < 120 * 25; ++i) {
        r.step(calm);
        if (i == 120 * 24) late = std::fabs(roll_of(r.world->state(r.body).rotation));
    }
    std::printf("  a wide flat box heeled 25 degrees: roll %.1f degrees at the start, %.2f after 24 s\n", first * 180.0 / pi, late * 180.0 / pi);
    require(late < 2.0 * pi / 180.0, "a stable box rights itself (under 2 degrees after 24 s)");
}

void test_drop() {
    const WaterSurface calm{WaterConfig{}};
    Rig r({0.5, 0.5, 0.25}, 600.0, {0, 0, 3.0});
    double deepest = 1e9, first_wet = -1.0;
    for (int i = 0; i < 120 * 25; ++i) {
        const BuoyancyResult b = r.step(calm);
        if (first_wet < 0.0 && b.wet_cells > 0) first_wet = r.t;
        deepest = std::min(deepest, r.world->state(r.body).position.z);
        require(std::isfinite(r.world->state(r.body).position.z), "stays finite");
    }
    const double settled = r.world->state(r.body).position.z;
    std::printf("  dropped from 3 m: first wet at %.2f s (free fall %.2f s), deepest centre %.2f m, settled at %.3f m (Archimedes -0.050)\n", first_wet, std::sqrt(2.0 * 2.75 / 9.81), deepest, settled);
    require(std::fabs(first_wet - std::sqrt(2.0 * 2.75 / 9.81)) < 0.03, "it reaches the water when free fall says");
    require(deepest < -0.2, "it plunges below its floating draft");
    require(std::fabs(settled - (-0.05)) < 0.015, "and settles at the draft");
}

void test_validation() {
    const WaterSurface calm{WaterConfig{}};
    Rig r({0.5, 0.5, 0.5}, 500.0, {0, 0, 0});
    const auto bad = [&](BuoyantBox b, double dt = 1.0 / 60.0) {
        try {
            (void)apply_buoyancy(*r.world, r.body, b, calm, 0.0, dt);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    BuoyantBox b = r.box;
    require(!bad(b), "control: a good box and step");
    BuoyantBox c = b;
    c.mass = 0.0;
    require(bad(c), "a zero mass is refused");
    c = b;
    c.cells = 1;
    require(bad(c), "one cell is refused");
    c = b;
    c.half_extents.x = -1.0;
    require(bad(c), "a negative size is refused");
    c = b;
    c.drag_coefficient = std::nan("");
    require(bad(c), "a NaN coefficient is refused");
    require(bad(b, 0.0), "a zero step is refused");
    bool threw = false;
    try {
        (void)apply_buoyancy(*r.world, BodyId{}, b, calm, 0.0, 1.0 / 60.0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a stale body is refused");
}

void test_water_motion() {
    // A current carries a floating body to its speed (the drag works against the water's flow, not against rest).
    {
        WaterConfig wc;
        wc.current = {2.0, 1.0, 0.0};
        const WaterSurface river(wc);
        Rig r({0.5, 0.5, 0.25}, 600.0, {0, 0, -0.05});
        for (int i = 0; i < 120 * 90; ++i) r.step(river);
        const DBodyState s = r.world->state(r.body);
        std::printf("  in a current of (2, 1) m/s the body reaches (%.3f, %.3f) m/s after 90 s\n", s.linear_velocity.x, s.linear_velocity.y);
        require(std::fabs(s.linear_velocity.x - 2.0) < 0.06 && std::fabs(s.linear_velocity.y - 1.0) < 0.06, "the body reaches the current's speed (within 3%)");
    }
    // Spin in the water slows (the cells' own motion meets the water), and does not when the rotational drag is turned off.
    for (const double angular_drag : {1.0, 0.0}) {
        const WaterSurface calm{WaterConfig{}};
        Rig r({0.4, 0.4, 0.4}, 1000.0, {0, 0, -3.0});
        r.box.angular_drag = angular_drag;
        r.world->set_velocity(r.body, {0, 0, 0}, {0, 0, 3.0});
        for (int i = 0; i < 120 * 3; ++i) r.step(calm);
        const double w = r.world->state(r.body).angular_velocity.z;
        std::printf("  spin 3 rad/s underwater, angular drag %.0f: %.3f rad/s after 3 s\n", angular_drag, w);
        if (angular_drag > 0.0) require(w < 0.7 * 3.0 && w > 0.0, "a spinning body slows in the water");
        else require(w > 0.95 * 3.0 * std::exp(-0.02 * 3.0) - 0.05, "with rotational drag off it does not (the world's own damping only)");
    }
    // A fast light body does not rebound off the water's resistance: its speed falls monotonically and never changes sign (the drag per step is limited to what stops the cell).
    {
        const WaterSurface calm{WaterConfig{}};
        Rig r({0.1, 0.1, 0.1}, 800.0, {0, 0, -2.0});
        r.box.drag_coefficient = 4.0;
        r.world->set_velocity(r.body, {40.0, 0, 0}, {0, 0, 0});
        double last = 40.0;
        bool monotonic = true, reversed = false;
        for (int i = 0; i < 120 * 3; ++i) {
            r.step(calm);
            const double v = r.world->state(r.body).linear_velocity.x;
            monotonic = monotonic && v <= last + 0.05;
            reversed = reversed || v < -0.01;
            last = v;
        }
        require(monotonic && !reversed, "a 40 m/s body in water slows without rebounding");
    }
    // Radiation damping: a body that pierces the surface gives energy to the waves, so a drop's bobbing dies faster with heave and roll damping than without.
    for (const double damping : {0.15, 0.0}) {
        const WaterSurface calm{WaterConfig{}};
        Rig r({0.5, 0.5, 0.25}, 600.0, {0, 0, 1.0});
        r.box.heave_damping = damping;
        double window = 0.0;
        for (int i = 0; i < 120 * 12; ++i) {
            r.step(calm);
            if (r.t > 8.0) window = std::max(window, std::fabs(r.world->state(r.body).position.z - (-0.05)));
        }
        std::printf("  heave damping %.2f: the bobbing between 8 s and 12 s after a drop reaches %.4f m\n", damping, window);
        if (damping > 0.0) require(window < 0.03, "with heave damping the bobbing has died away by 8 s");
        else require(window > 0.02 && window > 5.0 * 0.0038, "control: without it the box is still bobbing (at least five times the damped amplitude)");
    }
    for (const double damping : {0.08, 0.0}) {
        const WaterSurface calm{WaterConfig{}};
        const double angle = 25.0 * pi / 180.0;
        Rig r({1.0, 1.5, 0.2}, 500.0, {0, 0, 0}, {std::sin(angle / 2), 0, 0, std::cos(angle / 2)});
        r.box.roll_damping = damping;
        double window = 0.0;
        for (int i = 0; i < 120 * 16; ++i) {
            r.step(calm);
            if (r.t > 10.0) window = std::max(window, std::fabs(roll_of(r.world->state(r.body).rotation)));
        }
        std::printf("  roll damping %.2f: the roll between 10 s and 16 s reaches %.2f degrees\n", damping, window * 180.0 / pi);
        if (damping > 0.0) require(window < 0.5 * pi / 180.0, "with roll damping the rocking has died away by 10 s (half a degree)");
        else require(window > 1.0 * pi / 180.0, "control: without it the box still rocks (over 1 degree, against 0.01 damped)");
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
    const Named tests[] = {{"draft", test_draft}, {"sinking", test_sinking}, {"waves", test_waves}, {"righting", test_righting}, {"drop", test_drop}, {"motion", test_water_motion}, {"validation", test_validation}};
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
    if (failures == 0) std::cout << "buoyancy tests passed\n";
    return failures == 0 ? 0 : 1;
}
