// CPU cost baseline of the simulation pieces, per step, so a later change to storage, threading or a physics backend has a number to be compared with. Run it from a Release build:
//   saige_sim_baseline [--json]
// Prints the machine (CPU model, compiler, build type) with every table: a number is only comparable on the same machine. Single thread, no GPU: this says nothing about rendering.
#include "saige/capsule_character.hpp"
#include "saige/vehicle.hpp"
#include "saige/water.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace saige;
using Clock = std::chrono::steady_clock;

// Median of `repeats` runs of fn(), where fn performs `inner` operations: nanoseconds per operation.
template <class F>
double ns_per_op(int repeats, int inner, F&& fn) {
    std::vector<double> t;
    for (int r = 0; r < repeats; ++r) {
        const auto a = Clock::now();
        fn();
        const auto b = Clock::now();
        t.push_back(std::chrono::duration<double, std::nano>(b - a).count() / inner);
    }
    std::sort(t.begin(), t.end());
    return t[t.size() / 2];
}

std::string cpu_model() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
    }
    return "unknown";
}

struct Row {
    std::string name;
    double ns;
    std::string note;
};

} // namespace

int main(int argc, char** argv) {
    const bool json = argc > 1 && std::strcmp(argv[1], "--json") == 0;
    std::vector<Row> rows;
    volatile double sink = 0.0;

    // The vehicle: 1/60 s steps (two 1/120 s substeps each) on flat ground, and in a field of 2000 boxes it must query.
    for (const int field : {0, 2000}) {
        std::vector<Aabb> boxes{{{-1000, -1000, -2}, {1000, 1000, 0}}};
        for (int i = 0; i < field; ++i) {
            const float x = static_cast<float>((i % 50) * 30 - 700), y = static_cast<float>((i / 50) * 30 - 600);
            boxes.push_back({{x + 12, y + 12, 0}, {x + 14, y + 14, 3}});
        }
        const StaticCollision world(boxes, {});
        QueryScratch scratch;
        for (const bool full : {false, true}) {
            VehicleConfig cfg;
            if (full) {
                cfg.powertrain.enabled = true;
                cfg.powertrain.engine_brake = 90.0f;
                cfg.differential = VehicleDifferential::LimitedSlip;
                cfg.slip_angle_lateral = true;
                cfg.damage.enabled = true;
            }
            Vehicle v(cfg, {-240, 0, 0.9f}, 0.0f);
            VehicleInput in;
            in.throttle = 0.6f;
            const int steps = 6000;
            const double ns = ns_per_op(5, steps, [&] {
                for (int i = 0; i < steps; ++i) {
                    in.steer = (i / 120) % 2 ? 0.2f : -0.2f;
                    if (v.state().position.x > 200.0f) v.park();
                    v.step(in, 1.0f / 60.0f, world, scratch);
                }
            });
            sink = sink + static_cast<double>(v.state().position.x);
            rows.push_back({std::string("Vehicle::step (1/60 s) ") + (full ? "all options" : "defaults") + ", " + std::to_string(field) + " boxes", ns, "two 1/120 s substeps, four wheel probes, chassis overlap"});
        }
    }
    // The capsule character walking in a box field.
    {
        std::vector<Aabb> boxes{{{-500, -500, -2}, {500, 500, 0}}};
        for (int i = 0; i < 400; ++i) {
            const float x = static_cast<float>((i % 20) * 20 - 200), y = static_cast<float>((i / 20) * 20 - 200);
            boxes.push_back({{x + 8, y + 8, 0}, {x + 10, y + 10, 2}});
        }
        CapsuleCharacter c({0, 0, 0.01f}, boxes, CapsuleCharacterConfig{});
        CapsuleInput in;
        in.forward = 1.0f;
        const int steps = 20000;
        const double ns = ns_per_op(5, steps, [&] {
            for (int i = 0; i < steps; ++i) {
                in.forward = 1.0f;
                in.left = (i / 300) % 2 ? 0.5f : -0.5f;
                c.step(in, std::chrono::nanoseconds(16'666'667));
                if (c.state().feet.x > 250.0f || c.state().feet.y > 250.0f || c.state().feet.x < -250.0f || c.state().feet.y < -250.0f) c.reset();
            }
        });
        sink = sink + static_cast<double>(c.state().feet.x);
        rows.push_back({"CapsuleCharacter::step (1/60 s), 400 boxes", ns, "collide-and-slide, step-up, ground probe"});
    }
    // Water: surface queries and floating bodies.
    {
        WaterConfig wc;
        wc.waves = waves_from_wind(10.0, 0.3, 12);
        wc.choppiness = 0.4;
        const WaterSurface water(wc);
        const int n = 200000;
        double acc = 0.0;
        const double h = ns_per_op(5, n, [&] {
            for (int i = 0; i < n; ++i) acc += water.height(static_cast<double>(i % 1000), static_cast<double>(i / 1000), 3.0);
        });
        const double nrm = ns_per_op(5, n, [&] {
            for (int i = 0; i < n; ++i) acc += water.normal(static_cast<double>(i % 1000), static_cast<double>(i / 1000), 3.0).z;
        });
        sink = sink + acc;
        rows.push_back({"WaterSurface::height, 12 waves, choppiness 0.4", h, "includes the fixed-point inversion of the horizontal displacement"});
        rows.push_back({"WaterSurface::normal, same", nrm, ""});
        for (const int cells : {6, 12}) {
            FloatingBodyConfig bc;
            bc.half_extents = {1.0, 1.0, 0.4};
            bc.density = 500.0;
            bc.cells = cells;
            FloatingBody body(bc, {0, 0, 0});
            double t = 0.0;
            const int steps = cells == 6 ? 4000 : 1000;
            const double ns = ns_per_op(5, steps, [&] {
                for (int i = 0; i < steps; ++i) t = body.step(1.0 / 60.0, water, t);
            });
            sink = sink + body.state().position.z;
            rows.push_back({"FloatingBody::step (1/60 s), " + std::to_string(cells * cells * cells) + " cells, 12 waves", ns, "four 1/240 s substeps; one height query and one flow query per wet cell"});
        }
    }
    (void)sink;
    const std::string cpu = cpu_model();
#ifdef NDEBUG
    const char* build = "optimized (NDEBUG)";
#else
    const char* build = "NOT optimized: do not use these numbers";
#endif
    if (json) {
        std::printf("{\n \"cpu\": \"%s\",\n \"build\": \"%s\",\n \"unit\": \"ns per operation, median of 5\",\n \"rows\": [\n", cpu.c_str(), build);
        for (std::size_t i = 0; i < rows.size(); ++i) std::printf("  {\"name\": \"%s\", \"ns\": %.0f}%s\n", rows[i].name.c_str(), rows[i].ns, i + 1 < rows.size() ? "," : "");
        std::printf(" ]\n}\n");
    } else {
        std::printf("CPU: %s\nbuild: %s\nsingle thread; nanoseconds per operation, median of 5 runs\n\n", cpu.c_str(), build);
        for (const Row& r : rows) std::printf("%-70s %10.0f ns  %s\n", r.name.c_str(), r.ns, r.note.c_str());
    }
    return 0;
}
