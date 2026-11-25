// Showcase 04 "A World Alive": the living world on the district. Residents with homes, jobs and routines walk the streets and go into the shops; they stop at kerbs and cross only on the walk signal; they avoid
// each other and the people already in a doorway; the world simulation (world_sim) decides where each one wants to be, and the engine's physics decides where each one actually is: every pedestrian is a
// CapsuleCharacter (so a wall, a step, a stair or a parked car stops them as it stops the player), and the crowd solver only proposes the velocity the capsule is then driven with. Nothing here is a scripted path
// of a film: the schedules, the signal cycle and the crowd produce the scene, and a camera can look at it from anywhere.
//
// Limits, stated: buildings that have no modelled interior swallow their visitors at the door (the person is hidden, counted as inside, and comes out when the schedule says so); pedestrians are capsules (the
// animation layer reads their velocity and state; ragdolls and the stumbling recovery of CHAR-3 are separate work); the traffic side lives in vehicles.hpp.
#pragma once

#include "district.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace saige::alive {

struct WorldConfig {
    std::uint32_t seed = 7;
    int residents = 60;                 // pedestrians in the district (workers, shoppers, strollers)
    double start_minute = 8 * 60 + 30;  // time of day at the start
    double time_scale = 20.0;           // game seconds per real second (the simulation's clock: 20 makes a six-minute film span two game hours)
};

enum class PedState : std::uint8_t {
    Walking,        // following a route (the street, the promenade, a pier, a floor of a building)
    WaitingToCross, // at the kerb, the signal is against them
    Crossing,       // on a crosswalk
    Dwelling,       // standing at a destination (a counter, a bench, a table, the harbor rail)
    Indoors,        // inside a building with no modelled interior: hidden, still part of the venue's occupancy
    Fleeing,        // running from a danger the world reported
};

struct PedView {
    std::uint64_t id = 0;
    double x = 0, y = 0, z = 0;
    double yaw = 0;       // radians, +X east, counter-clockwise
    double speed = 0;     // m/s over the ground
    PedState state = PedState::Walking;
    bool visible = true;
    bool inside_building = false;  // in a modelled interior (visible, but under a roof)
    int floor = 0;
    world_sim::OccupationKind occupation = world_sim::OccupationKind::Unemployed;
    world_sim::NpcIntent intent = world_sim::NpcIntent::Idle;
    int palette = 0;      // clothing colour family for the renderer
    double stalled_s = 0; // seconds since the pedestrian last moved half a metre
    double target_x = 0, target_y = 0;  // the waypoint being walked to (for debug views)
};

struct WorldStats {
    std::uint32_t pedestrians = 0, walking = 0, waiting = 0, crossing = 0, dwelling = 0, indoors = 0, fleeing = 0;
    std::uint64_t crossings_started = 0;
    std::uint64_t crossings_against_signal = 0;   // must stay zero: nobody steps onto a crosswalk without the walk signal
    std::uint64_t venue_arrivals = 0;
    std::uint64_t interior_arrivals = 0;           // arrivals at a venue by walking into a modelled interior
    std::uint64_t stair_climbs = 0;                // arrivals on the upper floor of the cafe
    std::uint32_t overlapping_with_geometry = 0;   // pedestrians whose capsule intersects solid geometry now (must be zero)
    std::uint32_t stuck = 0;                       // walking pedestrians that have not moved for 100 s (longer than a full signal cycle: waiting in a queue at a kerb is not stuck)
    double min_separation_m = 0;                   // the crowd solver's closest pair (edge to edge)
    double ms_sim = 0, ms_decide = 0, ms_crowd = 0, ms_bodies = 0, ms_stats = 0;   // wall-clock milliseconds spent so far in each stage (the performance report reads these)
    double game_minute = 0;
    std::uint32_t day = 0;
};

class World {
public:
    explicit World(WorldConfig config = {});
    ~World();
    World(const World&) = delete;
    World& operator=(const World&) = delete;

    // Advances the world by `dt` seconds of real time (internally in fixed 1/60 s steps; the remainder carries over). `focus` is where the camera is: it sets which residents are simulated in full.
    void step(double dt);
    void set_focus(double x, double y);

    [[nodiscard]] const District& district() const;
    [[nodiscard]] double time_seconds() const;
    [[nodiscard]] double minute_of_day() const;
    [[nodiscard]] std::vector<PedView> pedestrians() const;
    [[nodiscard]] WorldStats stats() const;
    // Which walk signal the crosswalk shows now (the renderer draws the lamps from it).
    [[nodiscard]] bool walk_signal(std::uint32_t crosswalk) const;

    // The world reports a danger at a place: the residents near it hear it and react (the simulation's event machinery); used by the accident logic and by tests.
    void report_danger(double x, double y, double radius_m, double duration_s);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saige::alive
