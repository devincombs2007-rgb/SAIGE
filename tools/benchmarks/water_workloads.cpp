// Floating-body workloads of 1, 10, 50 and 100 bodies in a wind sea, with the ways of making them cheaper side by side: a shared surface cache, filtered waves for far bodies, fewer sample cells for far bodies and threads,
// and for each the cost per frame and the error against the exact reference (same sea, same bodies, 216 cells, every wave). Run from a Release build:  saige_water_workloads [--accuracy-only | --cost-only]
#include "saige/water.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

using namespace saige;
using Clock = std::chrono::steady_clock;

namespace {

struct Placed {
    double x, y, distance;
};

std::vector<Placed> place(int n) {
    std::vector<Placed> out;
    std::uint32_t s = 12345u;
    const auto rnd = [&] {
        s = s * 1664525u + 1013904223u;
        return static_cast<double>(s >> 8) / 16777216.0;
    };
    for (int i = 0; i < n; ++i) {
        const double x = (rnd() - 0.5) * 300.0, y = (rnd() - 0.5) * 300.0;
        out.push_back({x, y, std::hypot(x, y)});
    }
    return out;
}

struct Variant {
    const char* name;
    bool cache, lod_waves, adaptive_cells;
    unsigned threads;
};

struct World {
    WaterSurface full;
    WaterSurface mid, far;  // the largest 8 and the largest 4 waves
    WaterFieldCache cache_full, cache_mid, cache_far;
    std::vector<FloatingBody> bodies;
    std::vector<Placed> where;
    std::vector<int> band;  // 0 near, 1 mid, 2 far
    World(const WaterConfig& c, int n, const Variant& v)
        : full(c), mid(full.reduced(8)), far(full.reduced(4)), cache_full(full), cache_mid(mid), cache_far(far), where(place(n)) {
        for (const Placed& p : where) {
            FloatingBodyConfig bc;
            bc.half_extents = {1.0, 1.0, 0.4};
            bc.density = 500.0;
            bc.cells = 6;
            bodies.emplace_back(bc, WaterVec{p.x, p.y, 0.0});
            const int b = p.distance < 40.0 ? 0 : (p.distance < 100.0 ? 1 : 2);
            band.push_back(b);
            if (v.adaptive_cells) bodies.back().set_cells(b == 0 ? 6 : (b == 1 ? 4 : 3));
        }
    }
};

WaterConfig sea() {
    WaterConfig c;
    c.waves = waves_from_wind(10.0, 0.3, 12);
    c.choppiness = 0.4;
    return c;
}

// One frame: returns t + dt.
double frame(World& w, const Variant& v, double t, double dt) {
    std::vector<BodyStep> steps;
    if (v.cache) {
        w.cache_full.begin(t, t + dt);
        if (v.lod_waves) {
            w.cache_mid.begin(t, t + dt);
            w.cache_far.begin(t, t + dt);
        }
    }
    for (std::size_t i = 0; i < w.bodies.size(); ++i) {
        const int b = v.lod_waves ? w.band[i] : 0;
        const WaterSource* src = b == 0 ? static_cast<const WaterSource*>(&w.full) : (b == 1 ? static_cast<const WaterSource*>(&w.mid) : static_cast<const WaterSource*>(&w.far));
        if (v.cache) {
            WaterFieldCache& c = b == 0 ? w.cache_full : (b == 1 ? w.cache_mid : w.cache_far);
            c.cover(w.where[i].x, w.where[i].y, 4.0);
            src = &c;
        }
        steps.push_back({&w.bodies[i], src});
    }
    return step_bodies(steps, dt, t, v.threads);
}

} // namespace

int main(int argc, char** argv) {
    const bool accuracy_only = argc > 1 && std::strcmp(argv[1], "--accuracy-only") == 0, cost_only = argc > 1 && std::strcmp(argv[1], "--cost-only") == 0;
    const WaterConfig config = sea();
    const Variant variants[] = {{"reference: exact, 216 cells, serial", false, false, false, 1},
                                {"shared surface cache", true, false, false, 1},
                                {"distance: fewer waves (8 and 4 of 12)", false, true, false, 1},
                                {"distance: fewer cells", false, false, true, 1},
                                {"cache + fewer waves + fewer cells", true, true, true, 1},
                                {"all of that on 4 threads", true, true, true, 4},
                                {"exact reference on 4 threads", false, false, false, 4}};
    if (!cost_only) {
        // Accuracy: 20 bodies for 20 s from the same start; the error of each variant's height and roll against the reference, near bodies (under 40 m) and the rest.
        const int n = 20;
        const double dt = 1.0 / 60.0;
        std::vector<std::vector<double>> z_ref, r_ref;
        std::printf("accuracy against the exact reference (%d bodies, 20 s, wind sea of Hs about 2 m): RMS error of the body height and roll\n", n);
        std::printf("%-44s %14s %14s %14s %14s\n", "variant", "near z (mm)", "near roll (deg)", "far z (mm)", "far roll (deg)");
        for (std::size_t vi = 0; vi < sizeof(variants) / sizeof(variants[0]); ++vi) {
            const Variant& v = variants[vi];
            World w(config, n, v);
            std::vector<std::vector<double>> zs(static_cast<std::size_t>(n)), rs(static_cast<std::size_t>(n));
            double t = 0.0;
            for (int f = 0; f < 60 * 20; ++f) {
                t = frame(w, v, t, dt);
                if (f % 6 == 0) {
                    for (int i = 0; i < n; ++i) {
                        zs[static_cast<std::size_t>(i)].push_back(w.bodies[static_cast<std::size_t>(i)].state().position.z);
                        rs[static_cast<std::size_t>(i)].push_back(w.bodies[static_cast<std::size_t>(i)].roll() * 57.29578);
                    }
                }
            }
            if (vi == 0) {
                z_ref = zs;
                r_ref = rs;
            }
            double zn = 0, rn = 0, zf = 0, rf = 0;
            std::size_t cn = 0, cf = 0;
            for (int i = 0; i < n; ++i) {
                for (std::size_t k = 0; k < zs[static_cast<std::size_t>(i)].size(); ++k) {
                    const double dz = zs[static_cast<std::size_t>(i)][k] - z_ref[static_cast<std::size_t>(i)][k], dr = rs[static_cast<std::size_t>(i)][k] - r_ref[static_cast<std::size_t>(i)][k];
                    if (w.band[static_cast<std::size_t>(i)] == 0) zn += dz * dz, rn += dr * dr, ++cn;
                    else zf += dz * dz, rf += dr * dr, ++cf;
                }
            }
            std::printf("%-44s %14.2f %14.3f %14.2f %14.3f\n", v.name, cn ? std::sqrt(zn / static_cast<double>(cn)) * 1000.0 : 0.0, cn ? std::sqrt(rn / static_cast<double>(cn)) : 0.0, cf ? std::sqrt(zf / static_cast<double>(cf)) * 1000.0 : 0.0,
                        cf ? std::sqrt(rf / static_cast<double>(cf)) : 0.0);
            std::fflush(stdout);
        }
        std::printf("(near: %s bodies of %d within 40 m of the viewer)\n\n", "some", n);
    }
    if (!accuracy_only) {
        std::printf("cost per frame (1/60 s of simulation), milliseconds, Release, this machine; %s\n", "median of 3 timed runs of 60 frames after 60 warm-up frames");
        std::printf("%-44s %9s %9s %9s %9s\n", "variant", "1 body", "10", "50", "100");
        for (const Variant& v : variants) {
            std::printf("%-44s", v.name);
            for (const int n : {1, 10, 50, 100}) {
                World w(config, n, v);
                double t = 0.0;
                for (int f = 0; f < 60; ++f) t = frame(w, v, t, 1.0 / 60.0);
                std::vector<double> runs;
                for (int r = 0; r < 3; ++r) {
                    const auto a = Clock::now();
                    for (int f = 0; f < 60; ++f) t = frame(w, v, t, 1.0 / 60.0);
                    runs.push_back(std::chrono::duration<double, std::milli>(Clock::now() - a).count() / 60.0);
                }
                std::sort(runs.begin(), runs.end());
                std::printf(" %9.3f", runs[1]);
                std::fflush(stdout);
            }
            std::printf("\n");
        }
    }
    return 0;
}
