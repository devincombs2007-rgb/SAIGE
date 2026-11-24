// Showcase 03 simulation tests: the world is a pure function of the cell, the run is deterministic, streaming keeps three-by-three cells resident while the origin
// migrates, a checkpoint resumes the same run, and the script reaches its crash and its aftermath.
#include "sim.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace saige::horizon;

namespace {
int failures = 0;
void check(bool ok, const char* what)
{
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

void run_to(Sim& sim, double seconds)
{
    while (sim.time() + 1e-9 < seconds) sim.tick();
}
} // namespace

int main()
{
    const World world;

    // A cell is a pure function of its coordinates.
    {
        const CellData a = world.build_cell(2, 0), b = world.build_cell(2, 0);
        check(a.vertex_count() == b.vertex_count() && a.triangle_count() == b.triangle_count() && a.triangles.size() == b.triangles.size() && a.props.size() == b.props.size(), "cells are reproducible");
        check(a.vertex_count() > 1000, "a town cell has geometry");
        double sum_a = 0, sum_b = 0;
        for (const MeshPart& p : a.parts) for (const Vertex& v : p.vertices) sum_a += static_cast<double>(v.px) + 2.0 * static_cast<double>(v.py) + 3.0 * static_cast<double>(v.pz);
        for (const MeshPart& p : b.parts) for (const Vertex& v : p.vertices) sum_b += static_cast<double>(v.px) + 2.0 * static_cast<double>(v.py) + 3.0 * static_cast<double>(v.pz);
        check(sum_a == sum_b, "cell vertices are bit-identical");
        // Neighbouring cells meet: the height field is continuous across a cell edge.
        const double h0 = world.height(127.999, 20.0), h1 = world.height(128.001, 20.0);
        check(std::fabs(h0 - h1) < 0.01, "terrain is continuous across a cell edge");
    }

    // The run is deterministic and the streaming keeps nine cells resident.
    std::uint64_t hash_a = 0;
    {
        Sim a(world), b(world);
        run_to(a, 70.0);
        run_to(b, 70.0);
        hash_a = a.state_hash();
        check(a.state_hash() == b.state_hash(), "two runs of the same script have the same state hash");
        check(a.driving(), "the player is driving at 70 s");
        check(a.vehicle() != nullptr && a.vehicle()->state().forward_speed > 12.0f, "the car is moving at speed");
        check(a.resident().size() == 9, "nine cells are resident");
        check(a.stream_stats().origin_migrations >= 5, "the origin migrated as the car crossed cells");
        const double d = std::hypot(a.origin().x - std::floor((a.vehicle()->state().position.x + a.origin().x) / cell_size) * cell_size, 0.0);
        (void)d;
        const auto& s = a.vehicle()->state().position;
        check(std::fabs(s.x) < 200.0f && std::fabs(s.y) < 200.0f, "local coordinates stay small");
    }

    // A checkpoint is a save game: poses, the script's phase and the damage (not velocities), so it is taken where the world is calm (the walker approaching the parked car) and
    // resuming it continues the run: the character reaches the car, climbs in and drives off, within a few meters of the uninterrupted run.
    {
        Sim a(world);
        run_to(a, 36.0);
        const std::string json = a.checkpoint_json();
        std::unique_ptr<Sim> b = Sim::from_checkpoint(world, json);
        check(b != nullptr, "a checkpoint loads");
        if (b) {
            check(std::fabs(b->time() - a.time()) < 1e-9 && b->phase() == a.phase(), "the checkpoint restores the clock and the phase");
            run_to(a, 44.0);
            run_to(*b, 44.0);
            const auto feet = [](const Sim& s) {
                const auto f = s.character_feet();
                return D2{f.x + s.origin().x, f.y + s.origin().y};
            };
            const D2 pa = feet(a), pb = feet(*b);
            const double d = std::hypot(pa.x - pb.x, pa.y - pb.y);
            std::printf("checkpoint: the resumed walker is %.2f m from the uninterrupted run after 8 s\n", d);
            check(d < 3.0, "the resumed run follows the continuous one");
            check(b->phase() == a.phase(), "both runs are in the same phase");
        }
    }

    // The crash and what follows.
    {
        Sim a(world);
        run_to(a, 105.0);
        bool barrier = false;
        for (const ImpactEvent& e : a.impacts()) barrier = barrier || e.barrier;
        check(barrier, "the car hit the barrier");
        check(a.damage() > 0.2f, "the car is damaged");
        check(!a.driving() && a.vehicle() != nullptr && std::fabs(a.vehicle()->state().forward_speed) < 0.5f, "the wreck is at rest");
        bool debris = false;
        for (const PropBody& p : a.bodies()) debris = debris || p.debris;
        check(debris, "the impact threw debris");
        check(a.character_visible(), "the driver climbed out");
    }
    std::printf("hash at 70 s: %016llx\n", static_cast<unsigned long long>(hash_a));
    return failures == 0 ? 0 : 1;
}
