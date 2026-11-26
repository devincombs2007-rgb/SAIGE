// Showcase 04: the director's camera on whole films, without a renderer. For each (seed, report) the film's 5400-frame camera path is walked the way the film does (25 s of pre-roll, then one world step per frame,
// the director asked for the camera) and every frame is judged by rules that do not share code with the director's search except the geometry predicates themselves:
//   * the eye is not inside a wall or prop (eye_in_solid), not inside leaves or an awning and does not look through them (view_obstructed), and a building does not stand between it and the subject a shot follows;
//   * the eye is within the displacement budget of what the shot proposed (measured here from Director::last_proposed_eye, not from the director's own statistics), except frames counted as beyond the budget to leave a wall;
//   * no frame fails any rule without being counted by the director as unresolved or beyond budget (the director does not hide a failure);
//   * the eye does not jump: the change of (final eye - proposed eye) between consecutive frames of one shot stays below a stated limit.
// Each seed is walked with the report scouted for it and with a report without an accident (the two give different shot lists; the first cross-seed check used only the second by mistake, see scout_test.cpp).
// Usage: alive_camera_test [--all | seed:real|seed:none ...]. Default: four cases chosen because each failed in an earlier version of the director (3:real and 2:real, an eye inside a wall; 6:real, an accident in the
// first 49 s; 34:none, foliage on the seed the coordinator reported). --all walks 8 seeds x 2 reports. Exit status 0 when every walked case passes.
#include "district_validate.hpp"
#include "director.hpp"
#include "scout.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace saige::alive;

namespace {

struct Case {
    std::uint32_t seed;
    bool real;
};

struct Result {
    int frames = 0, obstructed = 0, solid = 0, hidden = 0, bad = 0;
    int over_ceiling = 0;   // frames whose displacement from the proposal exceeds the budget (5 m horizontal, 2.5 m up) although the director did not count them as beyond the budget
    double max_h = 0, max_lift = 0, max_jump = 0;
    int pops = 0;
    rend::ClearanceStats stats;
    std::string worst_jump;
};

WorldConfig film_config(std::uint32_t seed) {
    WorldConfig c;
    c.seed = seed;
    c.time_scale = 47.0;
    c.start_minute = 17.5 * 60.0;
    return c;
}

Result walk(const Case& cs) {
    const WorldConfig wc = film_config(cs.seed);
    ScoutReport rep;
    if (cs.real) rep = scout_world(wc, 25.0 + 180.0 + 1.0);
    else rep.accident_t = rep.onscene_t = rep.cleared_t = -1.0;
    World world(wc);
    const District& d = world.district();
    rend::Director director(d, rep, 180.0, 25.0);
    std::vector<StreetProp> trees;
    for (const StreetProp& p : d.props)
        if (p.type == "tree") trees.push_back(p);
    const std::vector<Box3> awnings = awning_boxes(d);
    const double dt = 1.0 / 30.0;
    for (double t = 0.0; t < 25.0; t += dt) world.step(dt);
    Result r;
    std::string prev_shot;
    float prev_off[3] = {0, 0, 0};
    int over_budget_seen = 0;
    for (int f = 0; f < 5400; ++f) {
        const double tf = static_cast<double>(f) * dt;
        world.step(dt);
        const rend::Cam cam = director.camera(tf, dt, world);
        const rend::Float3 p = director.last_proposed_eye();
        const std::array<float, 3> eye{cam.eye.x, cam.eye.y, cam.eye.z}, look{cam.look.x, cam.look.y, cam.look.z};
        const bool obstructed = view_obstructed(trees, awnings, eye, look, cam.fov_deg);
        const bool solid = eye_in_solid(d, eye) || view_into_solid(d, eye, look, 1.2f);   // inside a wall, or a wall within 1.2 m straight ahead
        const std::array<float, 3> target = cam.has_subject ? std::array<float, 3>{cam.subject.x, cam.subject.y, cam.subject.z} : look;
        const bool hidden = director.shot_tracks_subject(tf) && sightline_blocked_by_building(d, eye, target);
        ++r.frames;
        r.obstructed += obstructed;
        r.solid += solid;
        r.hidden += hidden;
        r.bad += (obstructed || solid || hidden) ? 1 : 0;
        const float off[3] = {cam.eye.x - p.x, cam.eye.y - p.y, cam.eye.z - p.z};
        const double h = std::hypot(static_cast<double>(off[0]), static_cast<double>(off[1]));
        r.max_h = std::max(r.max_h, h);
        r.max_lift = std::max(r.max_lift, static_cast<double>(off[2]));
        if (h > 5.0 + 1e-3 || off[2] > 2.5 + 1e-3) ++over_budget_seen;
        const std::string shot = director.shot_name(tf);
        if (shot == prev_shot) {
            const double jump = std::sqrt(static_cast<double>((off[0] - prev_off[0]) * (off[0] - prev_off[0]) + (off[1] - prev_off[1]) * (off[1] - prev_off[1]) + (off[2] - prev_off[2]) * (off[2] - prev_off[2])));
            if (jump > 1.5) ++r.pops;
            if (jump > r.max_jump) {
                r.max_jump = jump;
                r.worst_jump = "frame " + std::to_string(f) + " in '" + shot + "'";
            }
        }
        prev_shot = shot;
        for (int k = 0; k < 3; ++k) prev_off[k] = off[k];
    }
    r.stats = director.clearance_stats();
    r.over_ceiling = std::max(0, over_budget_seen - r.stats.over_budget);
    return r;
}

int failures = 0;

#define CHECK(cond, ...)                                                                                      \
    do {                                                                                                      \
        if (!(cond)) {                                                                                        \
            std::printf("FAIL %s:%d: %s -- ", __FILE__, __LINE__, #cond);                                     \
            std::printf(__VA_ARGS__);                                                                         \
            std::printf("\n");                                                                                \
            ++failures;                                                                                       \
        }                                                                                                     \
    } while (0)

} // namespace

int main(int argc, char** argv) {
    std::vector<Case> cases;
    if (argc == 2 && std::string(argv[1]) == "--all") {
        for (const std::uint32_t seed : {8u, 34u, 1u, 2u, 3u, 4u, 5u, 6u})
            for (const bool real : {true, false}) cases.push_back({seed, real});
    } else if (argc > 1) {
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            const auto colon = a.find(':');
            if (colon == std::string::npos || (a.substr(colon + 1) != "real" && a.substr(colon + 1) != "none")) {
                std::printf("bad case '%s' (want seed:real or seed:none)\n", a.c_str());
                return 2;
            }
            cases.push_back({static_cast<std::uint32_t>(std::atoi(a.substr(0, colon).c_str())), a.substr(colon + 1) == "real"});
        }
    } else {
        cases = {{3, true}, {2, true}, {6, true}, {34, false}};
    }
    int walked = 0;
    for (const Case& cs : cases) {
        const Result r = walk(cs);
        ++walked;
        std::printf("seed %2u %s: %d frames, bad %d (obstructed %d, solid %d, hidden %d), displaced %d, shift %.2f m horizontal %.2f m up, beyond budget %d, re-framed %d, unresolved %d, largest change of the shift %.2f m, %d jumps over 1.5 m (%s)\n", cs.seed, cs.real ? "real " : "none ", r.frames, r.bad, r.obstructed, r.solid, r.hidden, r.stats.displaced_frames, r.max_h, r.max_lift, r.stats.over_budget, r.stats.reframed, r.stats.unresolved, r.max_jump, r.pops, r.worst_jump.c_str());
        CHECK(r.frames == 5400, "walked %d frames", r.frames);
        // Nothing fails silently: every failing frame is one the director counted as unresolved.
        CHECK(r.bad <= r.stats.unresolved, "seed %u %s: %d frames fail a rule but the director counted only %d as unresolved", cs.seed, cs.real ? "real" : "none", r.bad, r.stats.unresolved);
        // The budget is cumulative and independent of the number of passes: only frames the director counted as beyond the budget may exceed it.
        CHECK(r.over_ceiling == 0, "seed %u %s: %d frames moved the eye beyond 5 m / 2.5 m without being counted", cs.seed, cs.real ? "real" : "none", r.over_ceiling);
        // The camera that this slice's check accepts: no frame fails a rule.
        CHECK(r.bad == 0, "seed %u %s: %d frames fail (obstructed %d, solid %d, hidden %d)", cs.seed, cs.real ? "real" : "none", r.bad, r.obstructed, r.solid, r.hidden);
        CHECK(r.stats.unresolved == 0, "seed %u %s: %d frames had no passing candidate within the budget", cs.seed, cs.real ? "real" : "none", r.stats.unresolved);
        // The budget is met, not exceeded and then reported: no frame is beyond 5 m / 2.5 m (the director counts none beyond it, and the independent measurement above agrees) and none had to be re-framed on an arc.
        CHECK(r.stats.over_budget == 0 && r.stats.reframed == 0 && r.max_h <= 5.0 + 1e-3 && r.max_lift <= 2.5 + 1e-3, "seed %u %s: %d frames beyond the budget, %d re-framed, largest shift %.2f m, %.2f m up", cs.seed, cs.real ? "real" : "none", r.stats.over_budget, r.stats.reframed, r.max_h, r.max_lift);
        // Continuity, measured independently of the director's bookkeeping: the change of (final eye - proposed eye) between two frames of one shot. It is a KNOWN LIMIT, not a pass: when the standing offset stops
        // passing and the nearest passing one is far away the eye jumps (5.6 m on seed 4, in a follow shot of a resident). The test bounds it so that it cannot get worse unnoticed: at most 6 m in one frame
        // and at most 3 jumps over 1.5 m in a film (measured: at most 2 per film and 5.6 m). The director's own count must agree with the test's.
        CHECK(r.max_jump <= 6.0, "seed %u %s: the clearance moved the eye by %.2f m in one frame (%s)", cs.seed, cs.real ? "real" : "none", r.max_jump, r.worst_jump.c_str());
        CHECK(r.pops <= 3 && r.pops == r.stats.pops, "seed %u %s: %d jumps over 1.5 m in this film (the director counted %d)", cs.seed, cs.real ? "real" : "none", r.pops, r.stats.pops);
    }
    CHECK(walked > 0, "no case was walked");
    if (failures == 0) std::printf("camera tests passed (%d cases)\n", walked);
    return failures == 0 ? 0 : 1;
}
