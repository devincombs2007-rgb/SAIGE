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
    // For a shot that follows a resident or a vehicle: the subject itself (not the point the camera looks at, which is ahead of a car and of a walker). Visibility of the subject is judged on this.
    Float3 subject{0, 0, 0};
    bool has_subject = false;
};

/// The eye is moved from what a shot proposes only inside a bounded search (horizontal 5 m, up to 2.5 m up). `unresolved` counts the frames for which no candidate in that budget passed the rules: they are left at the
/// least-bad candidate and are reported by the film's camera check, never hidden. `max_offset_step` is the largest change of the offset between two consecutive frames (a jump of the camera that is not the shot's).
struct ClearanceStats {
    int unresolved = 0;
    int over_budget = 0;   // frames where only an eye beyond the budget (up to 12 m) cleared a wall or prop: the budget is exceeded for those frames, and counted here
    int displaced_frames = 0;
    int pops = 0;   // changes of the offset larger than 1.5 m between two consecutive frames of one shot: the eye had to jump because the standing offset stopped passing and the nearest passing one was far (a cut the shot did not ask for)
    float max_offset_horizontal = 0, max_offset_lift = 0, max_offset_step = 0;
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
    /// What the clearance search did over the calls so far.
    [[nodiscard]] ClearanceStats clearance_stats() const;
    /// The eye the last call to camera() started from, before any clearance (what the shot asked for): lets a test measure the displacement independently of the director's own bookkeeping.
    [[nodiscard]] Float3 last_proposed_eye() const;
    [[nodiscard]] std::vector<std::pair<double, std::string>> list() const;   // start time and name of every shot

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saige::alive::rend
