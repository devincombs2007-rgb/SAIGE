// Showcase 04: a headless pass over the world that records what happened and when (the accident, the response, who crossed where, who went into the cafe), so that the camera director can place its shots
// on events the simulation really produced. The pass runs the same fixed steps the film does, so the film replays it exactly.
#pragma once

#include "world.hpp"

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
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

// The scout file: the one text format the producers (saige_alive, alive_scout) write and the consumer (saige_alive's director) reads.
//
//   saige-scout 1
//   seed <uint>
//   accident <t> <x> <y> <vehicle> <onscene> <cleared>      exactly one; t, onscene, cleared are -1 ("did not happen") or > 0 (sim seconds)
//   event <t> <type 0..4> <vehicle> <x> <y> <energy>        zero or more, t non-decreasing
//   end <number of event lines>                             a truncated file has no end line or the wrong count
//
// "No accident in the scouted time" is a valid report (accident_t = -1 and no Accident event); an empty, truncated or malformed file is not, and
// is never read as one. Crossings and interior visits are scout outputs for humans and are not stored. The film of Showcase 04 was shot from a file
// in the headerless legacy layout (accident with six numbers, event with five numbers, no seed, no end line): it can only be read with
// allow_legacy, because that layout cannot detect truncation.
struct ScoutParse {
    bool ok = false;
    std::string error;     // what is wrong and on which line, when !ok
    bool legacy = false;   // the headerless layout was accepted
    std::uint32_t seed = 0;
    bool has_seed = false;
};

[[nodiscard]] ScoutParse parse_scout_report(std::string_view text, ScoutReport& out, bool allow_legacy = false);
void write_scout_report(std::ostream& out, const ScoutReport& report, std::uint32_t seed);

} // namespace saige::alive
