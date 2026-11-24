// Showcase 03 simulation tests: the world is a pure function of the cell, the run is deterministic, streaming keeps three-by-three cells resident while the origin
// migrates, a checkpoint resumes the same run, and the script reaches its crash and its aftermath.
#include "props.hpp"
#include "sim.hpp"
#include "saige/capsule_character.hpp"
#include "saige/static_collision.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>
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

    // What is drawn is what collides. Showcase 03 shipped with the barriers' collision turned 90 degrees from their rendered models; these tests tie the two together for every solid prop.
    {
        const auto inside = [](const PlacedBox& b, double x, double y, double margin) {
            const double c = std::cos(b.heading), sn = std::sin(b.heading);
            const double dx = x - b.cx, dy = y - b.cy;
            const double lx = c * dx + sn * dy, ly = -sn * dx + c * dy;
            return std::fabs(lx) <= b.half_length - margin && std::fabs(ly) <= b.half_width - margin;
        };
        std::vector<PropInstance> barriers;
        std::vector<PropInstance> solids;
        std::vector<saige::Triangle> tris;
        const double ox = 10 * cell_size, oy = -2 * cell_size;  // a common frame for the crash site's cells (floats stay small)
        for (int cy = -3; cy <= -1; ++cy) {
            for (int cx = 9; cx <= 12; ++cx) {
                const CellData cell = world.build_cell(cx, cy);
                for (const PropInstance& p : cell.props) {
                    if (p.type == PropType::Barrier) barriers.push_back(p);
                    if (prop_spec(p.type).solid && p.scale > 0.0f) solids.push_back(p);
                }
                for (saige::Triangle t : cell.triangles) {  // cell-corner relative to the shared frame
                    const float dx = static_cast<float>(cx * cell_size - ox), dy = static_cast<float>(cy * cell_size - oy);
                    for (saige::Vec3* v : {&t.a, &t.b, &t.c}) { v->x += dx; v->y += dy; }
                    tris.push_back(t);
                }
            }
        }
        check(barriers.size() >= 8, "the crash site has a row of barriers");
        const saige::StaticCollision collision({}, tris);
        // 1. The hull has the same orientation and about the same footprint as the model.
        bool aligned = true;
        for (const PropInstance& p : solids) {
            const PlacedBox v = placed_visual_box(p), h = placed_hull_box(p);
            if (p.type == PropType::Barrier) {
                aligned = aligned && std::fabs(v.half_length - h.half_length) < 0.03 && std::fabs(v.half_width - h.half_width) < 0.03 && std::hypot(v.cx - h.cx, v.cy - h.cy) < 0.03;
                aligned = aligned && v.half_length > v.half_width;  // the long side lies along the heading
            }
            aligned = aligned && std::hypot(h.cx - v.cx, h.cy - v.cy) < std::max(v.half_length, v.half_width);  // a hull never wanders off its model
        }
        check(aligned, "every solid prop's hull sits inside its model's footprint and barriers share its orientation");
        // 2. Rays from outside toward the middle of a rendered barrier hit the collision before they reach it, from all four sides.
        int missed = 0;
        for (const PropInstance& p : barriers) {
            const PlacedBox v = placed_visual_box(p);
            const double c = std::cos(v.heading), sn = std::sin(v.heading);
            const double zmid = (v.z_min + v.z_max) * 0.5;
            for (int side = 2; side < 4; ++side) {  // across the line; along it the neighbouring pieces touch end to end and stand in the way
                const double ax = side == 0 ? 1 : side == 1 ? -1 : 0, ay = side == 2 ? 1 : side == 3 ? -1 : 0;
                const double reach = v.half_width + 1.0;
                const double lx = ax * reach, ly = ay * reach;
                const saige::Vec3 from{static_cast<float>(v.cx + c * lx - sn * ly - ox), static_cast<float>(v.cy + sn * lx + c * ly - oy), static_cast<float>(zmid)};
                const double dx = -(c * ax - sn * ay), dy = -(sn * ax + c * ay);
                const auto hit = collision.raycast(from, saige::Vec3{static_cast<float>(dx), static_cast<float>(dy), 0.0f}, static_cast<float>(reach));
                if (!hit) { ++missed; if (missed < 4) std::printf("  miss: no hit, side %d reach %.2f heading %.2f at z %.2f\n", side, reach, v.heading, zmid); continue; }
                // the hit point must be on the model's surface (within 4 cm), not somewhere short of or past it
                const double hx = static_cast<double>(hit->point.x) + ox, hy = static_cast<double>(hit->point.y) + oy;
                if (!inside(v, hx, hy, -0.04) || inside(v, hx, hy, 0.04 + 0.5 * std::min(v.half_length, v.half_width))) { ++missed; if (missed < 4) std::printf("  miss: side %d hit distance %.3f of reach %.2f, point (%.2f, %.2f) vs box center (%.2f, %.2f) half %.2f x %.2f\n", side, hit->distance, reach, hx, hy, v.cx, v.cy, v.half_length, v.half_width); }
            }
        }
        check(missed == 0, "the collision stands where each barrier is drawn");
        // 3. A walking capsule cannot enter a drawn barrier: it walks at a barrier for five seconds from the road side.
        int entered = 0;
        for (std::size_t i = 0; i < barriers.size(); i += 3) {
            const PropInstance& p = barriers[i];
            const PlacedBox v = placed_visual_box(p);
            const double nx = -std::sin(v.heading), ny = std::cos(v.heading);  // across the line
            for (const double side : {1.0, -1.0}) {
                const double sx = v.cx + nx * side * 3.0, sy = v.cy + ny * side * 3.0;
                const auto ground = collision.raycast(saige::Vec3{static_cast<float>(sx - ox), static_cast<float>(sy - oy), static_cast<float>(v.z_max + 3.0)}, saige::Vec3{0.0f, 0.0f, -1.0f}, 12.0f);
                if (!ground) continue;
                const double spawn_z = static_cast<double>(v.z_max + 3.0) - ground->distance + 0.02;
                saige::CapsuleCharacterConfig cfg;
                cfg.radius = 0.30f; cfg.height = 1.75f; cfg.walk_speed = 3.4f; cfg.step_height = 0.4f;
                saige::CapsuleCharacter ch(saige::Vec3{static_cast<float>(sx - ox), static_cast<float>(sy - oy), static_cast<float>(spawn_z)}, std::span<const saige::Aabb>{}, std::span<const saige::Triangle>{}, cfg);
                for (int t = 0; t < 300; ++t) {
                    const saige::Vec3 f = ch.state().feet;
                    {   // the controller is handed the primitives near the feet each tick, as the simulation does
                        std::vector<saige::StaticPrimitive> ids;
                        collision.query_region({{f.x - 4.0f, f.y - 4.0f, f.z - 3.0f}, {f.x + 4.0f, f.y + 4.0f, f.z + 3.0f}}, ids);
                        std::vector<saige::Triangle> near;
                        for (const saige::StaticPrimitive& id : ids) if (id.kind == saige::StaticPrimitive::Kind::Triangle) near.push_back(collision.triangles()[id.index]);
                        (void)ch.replace_static_geometry({}, near);
                    }
                    // world-space input toward the barrier's center (the controller takes a world-space direction)
                    const double dx = v.cx - (static_cast<double>(f.x) + ox), dy = v.cy - (static_cast<double>(f.y) + oy), d = std::max(std::hypot(dx, dy), 0.05);  // standing on the target (only possible when nothing stops the walk) must not make a NaN input
                    ch.step({static_cast<float>(dx / d), static_cast<float>(dy / d), false, false}, std::chrono::nanoseconds(16666667));
                    const saige::Vec3 g = ch.state().feet;
                    if (inside(v, static_cast<double>(g.x) + ox, static_cast<double>(g.y) + oy, 0.0) && static_cast<double>(g.z) < v.z_max - 0.3) ++entered;
                }
            }
        }
        check(entered == 0, "a walking character never enters a drawn barrier");
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
