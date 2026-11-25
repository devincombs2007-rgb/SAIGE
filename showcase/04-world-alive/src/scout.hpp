// Showcase 04: a headless pass over the world that records what happened and when (the accident, the response, who crossed where, who went into the cafe), so that the camera director can place its shots
// on events the simulation really produced. The pass runs the same fixed steps the film does, so the film replays it exactly.
#pragma once

#include "world.hpp"

#include <cstdint>
#include <vector>

namespace saige::alive {

struct ScoutEvent {
    double t = 0;
    TrafficEvent::Type type = TrafficEvent::Type::Impact;
    std::uint32_t vehicle = 0;
    double x = 0, y = 0;
    float energy = 0;
};

struct ScoutCrossing {
    double t = 0;
    std::uint64_t ped = 0;
    double x = 0, y = 0;
};

struct ScoutVisit {
    double t = 0;
    std::uint64_t ped = 0;
    double x = 0, y = 0;
    int floor = 0;
};

struct ScoutReport {
    std::vector<ScoutEvent> events;
    std::vector<ScoutCrossing> crossings;
    std::vector<ScoutVisit> interior_visits;   // first moment each pedestrian was seen inside a modelled interior
    double accident_t = -1, accident_x = 0, accident_y = 0;
    std::uint32_t accident_vehicle = 0;
    double onscene_t = -1, cleared_t = -1;
};

// Steps `config`'s world in `dt` steps for `seconds` and reports. `frame_hook` (optional) is called after each step with the world and the time.
[[nodiscard]] ScoutReport scout_world(const WorldConfig& config, double seconds, double dt = 1.0 / 30.0);

} // namespace saige::alive
