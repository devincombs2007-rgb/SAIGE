// The physics backend evaluation: the same workloads through Jolt and Bullet behind one interface (backend.hpp). Prints one line per measurement and, with --json, a JSON document.
// Usage: physics_eval [--backend jolt|bullet|both] [--workload NAME|all] [--max-bodies N] [--json FILE]
// Workloads: stack, materials, ccd, chain, vehicle, determinism, scaling, largeworld, forces
#include "backend.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

using namespace eval;

namespace {

constexpr double kDt = 1.0 / 60.0;

struct Result {
    std::string backend, workload, metric;
    double value;
    std::string unit, note;
};
std::vector<Result> g_results;
void report(const std::string& backend, const std::string& workload, const std::string& metric, double value, const std::string& unit, const std::string& note = {}) {
    g_results.push_back({backend, workload, metric, value, unit, note});
    std::printf("%-12s %-12s %-34s %14.6g %-6s %s\n", backend.c_str(), workload.c_str(), metric.c_str(), value, unit.c_str(), note.c_str());
}

double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

long rss_kb() {
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) return std::stol(line.substr(6));
    }
    return 0;
}

Material kConcrete{0.6f, 0.2f};

void ground(Backend& b, Material m = {0.6f, 0.2f}, V3 at = {0, 0, 0}) { b.add_box({at + V3{0, 0, -0.5}, {500, 500, 0.5}, 0.0, m, {}, false}); }

// --- W1: a stack of 20 dynamic boxes.
void w_stack(Backend& b, const std::string& label, bool perturbed, bool heavy_top, V3 origin, int substeps = 1, int iterations = 10) {
    b.set_solver_iterations(iterations);
    const double t_start = now_ms();
    ground(b, kConcrete, origin);
    std::vector<int> ids;
    for (int i = 0; i < 20; ++i) {
        const double jitter = perturbed ? 0.01 * (i % 2 ? 1 : -1) : 0.0;
        const double mass = (heavy_top && i == 19) ? 200.0 : 20.0;
        ids.push_back(b.add_box({origin + V3{jitter, 0, 0.5 + i * 1.0}, {0.5, 0.5, 0.5}, mass, {0.6f, 0.0f}, {}, false}));
    }
    double max_speed_late = 0;
    for (int s = 0; s < 60 * 20; ++s) {
        b.step(kDt, substeps);
        if (s > 60 * 5) {
            for (int id : ids) max_speed_late = std::max(max_speed_late, length(b.velocity(id)));
        }
    }
    double drift = 0, height_err = 0;
    int asleep = 0;
    for (int i = 0; i < 20; ++i) {
        const V3 p = b.position(ids[static_cast<size_t>(i)]) - origin;
        drift = std::max(drift, std::hypot(p.x, p.y));
        asleep += b.active(ids[static_cast<size_t>(i)]) ? 0 : 1;
    }
    height_err = std::fabs((b.position(ids.back()) - origin).z - (0.5 + 19.0));
    const Q4 q = b.rotation(ids.back());
    const double tilt = 2.0 * std::acos(std::min(1.0, std::fabs(q.w)));
    report(b.name(), label, "max horizontal drift", drift, "m");
    report(b.name(), label, "top box height error", height_err, "m");
    report(b.name(), label, "top box tilt", tilt * 57.2958, "deg");
    report(b.name(), label, "max speed after 5 s", max_speed_late, "m/s");
    report(b.name(), label, "boxes asleep at 20 s", asleep, "of 20");
    report(b.name(), label, "cost per frame", (now_ms() - t_start) / (60.0 * 20.0), "ms", "includes the first seconds of motion; substeps " + std::to_string(substeps) + ", velocity iterations " + std::to_string(iterations));
}

// --- W2: materials: bounce apex and sliding distance against several floors; the SAIGE combination rule is the reference.
void w_materials(std::unique_ptr<Backend> (*make)(int), bool saige_rule) {
    struct Floor {
        const char* name;
        Material m;
    };
    const Floor floors[] = {{"rubber", {0.9f, 0.65f}}, {"concrete", {0.6f, 0.2f}}, {"ice", {0.05f, 0.1f}}, {"sand", {0.5f, 0.05f}}};
    const Material object{0.6f, 0.2f};
    for (const Floor& f : floors) {
        auto b = make(100);
        if (saige_rule) b->use_saige_material_rule();
        ground(*b, f.m);
        const int s = b->add_sphere({0, 0, 2.0}, 0.25, 1.0, object, false);
        double apex = 0;
        bool rising = false;
        double prev_z = b->position(s).z;
        for (int i = 0; i < 60 * 3 && !rising; ++i) {
            b->step(kDt, 2);
            if (b->velocity(s).z > 0.5) rising = true;
        }
        for (int i = 0; i < 60; ++i) {
            apex = std::max(apex, b->position(s).z - 0.25);
            b->step(kDt, 2);
            if (b->velocity(s).z < 0) break;
        }
        (void)prev_z;
        const double e = std::max<double>(object.restitution, f.m.restitution);
        const double expected = 1.75 * e * e;  // the ball starts 2.0 - 0.25 = 1.75 m above the surface
        const std::string label = std::string("materials") + (saige_rule ? "+rule" : "");
        report(b->name(), label, std::string("bounce apex on ") + f.name, apex, "m", "SAIGE rule expects " + std::to_string(expected));
        // Sliding: a box at 5 m/s along the floor; distance to stop against v^2 / (2 mu g) with mu = sqrt(mu_a mu_b).
        auto c = make(100);
        if (saige_rule) c->use_saige_material_rule();
        ground(*c, f.m);
        const int box = c->add_box({{0, 0, 0.25}, {0.25, 0.25, 0.25}, 5.0, {0.6f, 0.0f}, {}, false});
        c->set_velocity(box, {5, 0, 0});
        for (int i = 0; i < 60 * 30 && length(c->velocity(box)) > 0.01; ++i) c->step(kDt, 1);
        const double mu = std::sqrt(static_cast<double>(object.friction) * static_cast<double>(f.m.friction));
        report(c->name(), label, std::string("slide distance on ") + f.name, c->position(box).x, "m", "SAIGE rule expects " + std::to_string(25.0 / (2.0 * mu * 9.81)));
    }
}

// --- W3: a fast object against a thin wall (1 cm thick, at x = 22.3 so that no step lands exactly on it).
void w_ccd(Backend& b, bool ccd, double speed, bool box_shape) {
    ground(b);
    b.add_box({{22.3, 0, 1.0}, {0.005, 5.0, 2.0}, 0.0, {0.6f, 0.2f}, {}, false});
    const int p = box_shape ? b.add_box({{0, 0, 1.0}, {0.1, 0.1, 0.1}, 0.5, {0.6f, 0.2f}, {}, ccd}) : b.add_sphere({0, 0, 1.0}, 0.1, 0.5, {0.6f, 0.2f}, ccd);
    b.set_velocity(p, {speed, 0, 0.0});
    double max_x = 0;
    for (int i = 0; i < 60; ++i) {
        b.step(kDt, 1);
        max_x = std::max(max_x, b.position(p).x);
        // Hold the object against gravity and sideways drift so the line stays on the wall: the test is the pass through, not the arc.
        b.set_velocity(p, {b.velocity(p).x, 0, 0});
        if (max_x > 30.0) break;
    }
    report(b.name(), std::string("ccd ") + (box_shape ? "box" : "sphere") + (ccd ? " on" : " off"), "tunnelled through the wall", max_x > 22.5 ? 1.0 : 0.0, "bool", "speed " + std::to_string(static_cast<int>(speed)) + " m/s (" + std::to_string(speed * kDt) + " m per step), furthest x " + std::to_string(max_x));
}

// --- W4: an articulated chain of 10 links with swing-twist joints.
void w_chain(Backend& b) {
    ground(b, kConcrete, {0, 0, -5});
    const int n = 10;
    const double link = 0.4;
    std::vector<int> ids;
    // A fixed anchor and links lying along +X at height 3, released to swing down.
    const int anchor = b.add_box({{0, 0, 3.0}, {0.05, 0.05, 0.05}, 0.0, kConcrete, {}, false});
    ids.push_back(anchor);
    for (int i = 0; i < n; ++i) ids.push_back(b.add_box({{link * (i + 0.5), 0, 3.0}, {link * 0.5, 0.05, 0.05}, 2.0, {0.4f, 0.0f}, {}, false}));
    // Joints: the anchor to the first link and each link to the next, at the link ends; twist axis along the chain; swing +-45 degrees, twist +-20 degrees.
    for (int i = 0; i < n; ++i) b.add_swing_twist(ids[static_cast<size_t>(i)], ids[static_cast<size_t>(i) + 1], {link * i, 0, 3.0}, {1, 0, 0}, 0.785, -0.35, 0.35);
    double max_error = 0, max_speed = 0;
    for (int s = 0; s < 60 * 10; ++s) {
        b.step(kDt, 2);
        for (int i = 1; i < n; ++i) {
            const int a = ids[static_cast<size_t>(i)], c = ids[static_cast<size_t>(i) + 1];
            const V3 pa = b.position(a) + rotate(b.rotation(a), {link * 0.5, 0, 0});
            const V3 pb = b.position(c) + rotate(b.rotation(c), {-link * 0.5, 0, 0});
            max_error = std::max(max_error, length(pa - pb));
        }
        for (int i = 1; i <= n; ++i) max_speed = std::max(max_speed, length(b.velocity(ids[static_cast<size_t>(i)])));
    }
    // The swing angle between neighbouring links at the end, against the 45 degree limit.
    double worst_swing = 0;
    for (int i = 1; i < n; ++i) {
        const V3 da = rotate(b.rotation(ids[static_cast<size_t>(i)]), {1, 0, 0}), db = rotate(b.rotation(ids[static_cast<size_t>(i) + 1]), {1, 0, 0});
        worst_swing = std::max(worst_swing, std::acos(std::min(1.0, std::max(-1.0, dot(da, db)))));
    }
    report(b.name(), "chain", "max joint separation", max_error, "m", "10 links, 120 Hz");
    report(b.name(), "chain", "max link speed", max_speed, "m/s");
    report(b.name(), "chain", "final worst swing between links", worst_swing * 57.2958, "deg", "limit 45");
    report(b.name(), "chain", "final speed of the last link", length(b.velocity(ids.back())), "m/s", "settling");
}

// --- W5: a heavy fast "vehicle" body into a wall of movable crates: momentum, stability, no explosion.
void w_vehicle(Backend& b) {
    b.use_saige_material_rule();  // the same combination rule in both libraries, so the comparison is about the solver
    ground(b, {0.0f, 0.2f});      // frictionless floor (geometric-mean friction is then 0): momentum is lost to nothing but the collision itself
    const int car = b.add_box({{-8, 0, 0.5}, {2.15, 0.92, 0.48}, 1450.0, {0.05f, 0.1f}, {}, true});
    std::vector<int> crates;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 5; ++col) crates.push_back(b.add_box({{4.0, (col - 2) * 0.55, 0.25 + row * 0.5}, {0.25, 0.25, 0.25}, 20.0, {0.5f, 0.1f}, {}, false}));
    }
    b.set_velocity(car, {20, 0, 0});
    const double p0 = 1450.0 * 20.0;
    double max_speed = 0;
    for (int s = 0; s < 60 * 4; ++s) {
        b.step(kDt, 2);
        for (int id : crates) max_speed = std::max(max_speed, length(b.velocity(id)));
    }
    double p = 1450.0 * b.velocity(car).x;
    for (int id : crates) p += 20.0 * b.velocity(id).x;
    report(b.name(), "vehicle", "momentum after / before", p / p0, "ratio", "1.0 when conserved (frictionless floor)");
    report(b.name(), "vehicle", "fastest crate", max_speed, "m/s", "an elastic 72:1 hit would give up to 2x the car speed");
    report(b.name(), "vehicle", "car speed after", b.velocity(car).x, "m/s");
}

// --- Forces: a force at a point against the analytic angular acceleration (the semantics a vehicle's tire forces rely on).
void w_forces(Backend& b) {
    // Gravity off is not available through the interface: a body on a frictionless-ish support is awkward, so use a short free-fall comparison: the extra spin from an offset force over one step.
    const int box = b.add_box({{0, 0, 100}, {0.5, 0.5, 0.5}, 10.0, {0.5f, 0.0f}, {}, false});
    b.step(kDt, 1);
    const V3 w0 = b.angular_velocity(box), v0 = b.velocity(box);
    b.apply_force_at(box, {0, 100.0, 0}, b.position(box) + V3{0.5, 0, 0});  // 100 N along +Y at +0.5 m in X: torque about +Z = 0.5 * 100
    b.step(kDt, 1);
    const V3 w1 = b.angular_velocity(box), v1 = b.velocity(box);
    // Box inertia about Z: m (a^2 + b^2) / 12 = 10 * (1 + 1) / 12.
    const double inertia = 10.0 * 2.0 / 12.0;
    report(b.name(), "forces", "angular accel from an offset force", (w1.z - w0.z) / kDt, "rad/s2", "analytic " + std::to_string(0.5 * 100.0 / inertia));
    report(b.name(), "forces", "linear accel from the same force", (v1.y - v0.y) / kDt, "m/s2", "analytic 10");
}

// --- A pile of N boxes into a bin, for determinism and scaling.
struct Pile {
    std::vector<int> ids;
};
Pile make_pile(Backend& b, int n) {
    // A floor and four walls (a 12 m square bin), then n boxes dropped on a staggered grid.
    ground(b);
    for (int w = 0; w < 4; ++w) {
        const double sx = w < 2 ? 0.5 : 6.5, sy = w < 2 ? 6.5 : 0.5;
        const double cx = w == 0 ? -6.5 : (w == 1 ? 6.5 : 0), cy = w == 2 ? -6.5 : (w == 3 ? 6.5 : 0);
        b.add_box({{cx, cy, 25.0}, {sx, sy, 25.0}, 0.0, kConcrete, {}, false});
    }
    Pile p;
    const int per_layer = 100;
    for (int i = 0; i < n; ++i) {
        const int layer = i / per_layer, k = i % per_layer;
        const double x = (k % 10 - 4.5) * 1.1 + 0.07 * (layer % 3), y = (k / 10 - 4.5) * 1.1 + 0.05 * (layer % 5);
        p.ids.push_back(b.add_box({{x, y, 0.5 + layer * 1.15}, {0.5, 0.5, 0.5}, 10.0, {0.5f, 0.2f}, {}, false}));
    }
    return p;
}
uint64_t state_hash(Backend& b, const Pile& p) {
    uint64_t h = 1469598103934665603ull;
    for (int id : p.ids) {
        const V3 v = b.position(id);
        const float f[3] = {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
        for (float x : f) {
            uint32_t u;
            std::memcpy(&u, &x, 4);
            h = (h ^ u) * 1099511628211ull;
        }
    }
    return h;
}

void w_determinism(std::unique_ptr<Backend> (*make)(int)) {
    const auto run = [&](int threads) {
        auto b = make(2000);
        b->set_threads(threads);
        const Pile p = make_pile(*b, 300);
        for (int s = 0; s < 60 * 6; ++s) b->step(kDt, 1);
        return state_hash(*b, p);
    };
    const uint64_t a = run(1), b2 = run(1);
    report(make(10)->name(), "determinism", "same build, same threads: identical", a == b2 ? 1.0 : 0.0, "bool", "hash " + std::to_string(a));
    auto probe = make(10);
    if (probe->multithreaded()) {
        const uint64_t c = run(4);
        report(probe->name(), "determinism", "1 thread vs 4 threads: identical", a == c ? 1.0 : 0.0, "bool");
    } else {
        report(probe->name(), "determinism", "1 thread vs 4 threads: identical", -1.0, "n/a", "this build has no multithreading");
    }
}

void w_scaling(std::unique_ptr<Backend> (*make)(int), int max_n) {
    for (int n : {100, 500, 1000, 2000, 5000, 10000}) {
        if (n > max_n) break;
        for (int threads : {1, 4}) {
            auto b = make(n + 100);
            if (threads > 1 && !b->multithreaded()) continue;
            b->set_threads(threads);
            const long rss0 = rss_kb();
            const Pile p = make_pile(*b, n);
            double total = 0, last = 0;
            const int steps = 180;
            for (int s = 0; s < steps; ++s) {
                const double t0 = now_ms();
                b->step(kDt, 1);
                const double dt_ms = now_ms() - t0;
                total += dt_ms;
                if (s >= steps - 60) last += dt_ms;
            }
            const long rss1 = rss_kb();
            int active = 0;
            for (int id : p.ids) active += b->active(id) ? 1 : 0;
            report(b->name(), "scaling", "n=" + std::to_string(n) + " threads=" + std::to_string(threads) + " mean ms/step", total / steps, "ms", "last second " + std::to_string(last / 60.0) + " ms, active " + std::to_string(active) + ", +" + std::to_string((rss1 - rss0) / 1024) + " MiB");
        }
    }
}

// Precision far from the origin: a box resting on the ground and a stack of 5, at increasing distance. Height error against the exact rest height and the jitter of the resting box over 5 s.
void w_largeworld(std::unique_ptr<Backend> (*make)(int)) {
    for (double offset : {0.0, 256.0, 5000.0, 20000.0, 100000.0}) {
        auto b = make(100);
        const V3 origin{offset, offset * 0.5, 0};
        ground(*b, kConcrete, origin);
        const int single = b->add_box({origin + V3{0, 0, 0.6}, {0.5, 0.5, 0.5}, 20.0, {0.6f, 0.0f}, {}, false});
        std::vector<int> stack;
        for (int i = 0; i < 5; ++i) stack.push_back(b->add_box({origin + V3{3, 0, 0.5 + i}, {0.5, 0.5, 0.5}, 20.0, {0.6f, 0.0f}, {}, false}));
        double zmin = 1e9, zmax = -1e9, drift = 0;
        for (int s = 0; s < 60 * 8; ++s) {
            b->step(kDt, 2);
            if (s > 60 * 3) {
                const double z = (b->position(single) - origin).z;
                zmin = std::min(zmin, z);
                zmax = std::max(zmax, z);
            }
        }
        for (int id : stack) drift = std::max(drift, std::hypot((b->position(id) - origin).x - 3.0, (b->position(id) - origin).y));
        // A slow slide: 0.2 m/s on a frictionless floor for 5 s should cover 1.0 m (a step is 3.3 mm, below the float spacing at 100 km).
        auto c = make(100);
        ground(*c, {0.0f, 0.0f}, origin);
        const int slider = c->add_box({origin + V3{0, 0, 0.5}, {0.5, 0.5, 0.5}, 20.0, {0.0f, 0.0f}, {}, false});
        c->set_velocity(slider, {0.2, 0, 0});
        for (int s = 0; s < 60 * 5; ++s) c->step(kDt, 1);
        report(c->name(), "largeworld@" + std::to_string(static_cast<long>(offset)) + "m", "slow slide displacement", (c->position(slider) - origin).x, "m", "expected 1.0");
        const std::string label = "largeworld@" + std::to_string(static_cast<long>(offset)) + "m";
        report(b->name(), label, "resting box height error", std::fabs((zmin + zmax) * 0.5 - 0.5), "m");
        report(b->name(), label, "resting box jitter", zmax - zmin, "m");
        report(b->name(), label, "5-stack horizontal drift", drift, "m");
    }
}

} // namespace

int main(int argc, char** argv) {
    std::string which_backend = "both", which_workload = "all", json;
    int max_n = 10000;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--backend" && i + 1 < argc) which_backend = argv[++i];
        else if (a == "--workload" && i + 1 < argc) which_workload = argv[++i];
        else if (a == "--json" && i + 1 < argc) json = argv[++i];
        else if (a == "--max-bodies" && i + 1 < argc) max_n = std::stoi(argv[++i]);
    }
    struct Entry {
        const char* name;
        std::unique_ptr<Backend> (*make)(int);
    };
    std::vector<Entry> backends;
    if (which_backend != "bullet") backends.push_back({"jolt", make_jolt});
    if (which_backend != "jolt") backends.push_back({"bullet", make_bullet});
    const auto wants = [&](const char* w) { return which_workload == "all" || which_workload == w; };
    for (const Entry& e : backends) {
        std::printf("---- %s\n", e.name);
        if (wants("stack")) {
            struct Setting {
                int substeps, iterations;
            };
            for (const Setting st : {Setting{1, 10}, Setting{2, 10}, Setting{4, 10}, Setting{1, 30}, Setting{1, 60}}) {
                for (int variant = 0; variant < 3; ++variant) {
                    auto b = e.make(100);
                    const std::string label = std::string(variant == 0 ? "stack" : (variant == 1 ? "stack perturbed" : "stack heavy top")) + " x" + std::to_string(st.substeps) + "/i" + std::to_string(st.iterations);
                    w_stack(*b, label, variant == 1, variant == 2, {}, st.substeps, st.iterations);
                }
            }
        }
        if (wants("materials")) {
            w_materials(e.make, false);
            w_materials(e.make, true);
        }
        if (wants("ccd")) {
            for (bool ccd : {false, true}) {
                auto b = e.make(100);
                w_ccd(*b, ccd, 300.0, false);
                auto c = e.make(100);
                w_ccd(*c, ccd, 100.0, true);
            }
        }
        if (wants("chain")) {
            auto b = e.make(100);
            w_chain(*b);
        }
        if (wants("vehicle")) {
            auto b = e.make(200);
            w_vehicle(*b);
        }
        if (wants("forces")) {
            auto b = e.make(10);
            w_forces(*b);
        }
        if (wants("determinism")) w_determinism(e.make);
        if (wants("largeworld")) w_largeworld(e.make);
        if (wants("scaling")) w_scaling(e.make, max_n);
    }
    if (!json.empty()) {
        std::ofstream out(json);
        out << "[\n";
        for (size_t i = 0; i < g_results.size(); ++i) {
            const Result& r = g_results[i];
            out << "  {\"backend\": \"" << r.backend << "\", \"workload\": \"" << r.workload << "\", \"metric\": \"" << r.metric << "\", \"value\": " << r.value << ", \"unit\": \"" << r.unit << "\", \"note\": \"" << r.note << "\"}"
                << (i + 1 < g_results.size() ? "," : "") << "\n";
        }
        out << "]\n";
    }
    return 0;
}
