// A headless run of the Showcase 03 simulation: no renderer, no window. Prints the script's phase changes, the impacts and the streaming statistics, and writes the
// per-tick trace (used by the audio synthesis) and a checkpoint. Exit code 0 when the script reached its end.
#include "sim.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

using namespace saige::horizon;

int main(int argc, char** argv) {
    double seconds = 170.0;
    std::string out;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--seconds" && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
    }
    const World world;
    Sim sim(world);
    std::printf("route %.0f m, crash bend at s=%.0f (%.0f, %.0f)\n", world.route_length(), world.landmarks().crash_s, world.landmarks().crash_barrier.x, world.landmarks().crash_barrier.y);
    Phase last = sim.phase();
    std::size_t impacts = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const std::uint64_t ticks = static_cast<std::uint64_t>(seconds * 60.0);
    for (std::uint64_t i = 0; i < ticks; ++i) {
        sim.tick();
        if (sim.phase() != last) {
            last = sim.phase();
            const auto f = sim.character_feet();
            const auto w = sim.to_world(static_cast<double>(f.x), static_cast<double>(f.y));
            std::printf("t=%7.2f phase %-10s char world (%.1f, %.1f)", sim.time(), phase_name(last), w.x, w.y);
            if (sim.vehicle()) {
                const auto v = sim.to_world(static_cast<double>(sim.vehicle()->state().position.x), static_cast<double>(sim.vehicle()->state().position.y));
                std::printf("  car (%.1f, %.1f) %.1f m/s", v.x, v.y, static_cast<double>(sim.vehicle()->state().forward_speed));
            }
            std::printf("\n");
        }
        while (impacts < sim.impacts().size()) {
            const ImpactEvent& e = sim.impacts()[impacts++];
            std::printf("t=%7.2f IMPACT speed %.1f m/s energy %.0f J at (%.1f, %.1f, %.1f) normal (%.2f, %.2f, %.2f)%s\n", e.time, static_cast<double>(e.speed), static_cast<double>(e.energy), e.x, e.y, e.z,
                        static_cast<double>(e.nx), static_cast<double>(e.ny), static_cast<double>(e.nz), e.barrier ? " [barrier]" : "");
        }
        if (i % 600 == 0 && sim.vehicle()) {
            const auto v = sim.to_world(static_cast<double>(sim.vehicle()->state().position.x), static_cast<double>(sim.vehicle()->state().position.y));
            std::printf("  t=%6.1f car (%.1f, %.1f, %.2f) v=%.1f  driving=%d phase=%s\n", sim.time(), v.x, v.y, static_cast<double>(sim.vehicle()->state().position.z), static_cast<double>(sim.vehicle()->state().forward_speed), sim.driving() ? 1 : 0, phase_name(sim.phase()));
        }
        if (sim.phase() == Phase::Overlook && sim.time() > sim.timeline().overlook + 25.0) break;
    }
    const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const StreamStats& s = sim.stream_stats();
    std::printf("simulated %.1f s in %.1f s wall; phase %s; hash %016llx\n", sim.time(), wall, phase_name(sim.phase()), static_cast<unsigned long long>(sim.state_hash()));
    std::printf("streaming: %llu cells prepared, %llu prefetch hits, %llu blocking waits, %llu residency changes, %llu origin migrations, %zu resident cells, %llu resident collision triangles\n",
                static_cast<unsigned long long>(s.cells_prepared), static_cast<unsigned long long>(s.prefetch_hits), static_cast<unsigned long long>(s.blocking_waits), static_cast<unsigned long long>(s.residency_changes),
                static_cast<unsigned long long>(s.origin_migrations), s.resident_cells, static_cast<unsigned long long>(s.resident_collision_triangles));
    if (!out.empty()) {
        std::ofstream(out + "/checkpoint.json") << sim.checkpoint_json();
    }
    return sim.phase() == Phase::Overlook ? 0 : 1;
}
