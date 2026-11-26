// Showcase 04, the camera. The film has no scripted action: the director only chooses where the camera stands and what it follows. Shots follow residents and vehicles that really exist in the running
// simulation at that moment (so a "follow" shot is placed on whoever is there), and the accident block is placed on the moment the simulation produced it (the scout pass tells when).
#pragma once

#include "gfx.hpp"
#include "scout.hpp"
#include "world.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace saige::alive::rend {

struct Cam {
    Float3 eye{0, 0, 10};
    Float3 look{0, 10, 0};
    float fov_deg = 55.0f;
};

class Director {
public:
    // `film_seconds` is the film's length; `preroll` the simulated seconds that pass before the film's first frame; the accident block (if the report has one) is placed on its natural time.
    Director(const District& district, const ScoutReport& report, double film_seconds, double preroll);
    ~Director();
    // The camera at film time `t` (call with increasing t at a fixed step: the follow cameras are smoothed).
    [[nodiscard]] Cam camera(double t, double dt, const World& world);
    [[nodiscard]] std::string shot_name(double t) const;
    /// Does the shot at `t` follow a resident or a vehicle? (A place-based shot has no subject for a building to hide.)
    [[nodiscard]] bool shot_tracks_subject(double t) const;
    [[nodiscard]] std::vector<std::pair<double, std::string>> list() const;   // start time and name of every shot

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saige::alive::rend
