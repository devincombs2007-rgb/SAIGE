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
#include "vegetation_shape.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace saige::alive;

namespace {


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

// Analytic cases on a hand-made district: one 10 x 10 x 9.6 m building at x 0..10, y 0..10, whose collision box is the same. The expected answers are worked out by hand from the geometry, not by running the
// predicates on a film, so a predicate that is wrong in the same way as the director's use of it cannot hide here.
void analytic_cases() {
    District d;
    Building b;
    b.footprint = {0.0, 0.0, 10.0, 10.0};
    b.floors = 3;
    b.floor_height = 3.2;
    d.buildings.push_back(b);
    saige::Aabb wall;
    wall.min.x = 0.0f; wall.min.y = 0.0f; wall.min.z = 0.0f;
    wall.max.x = 10.0f; wall.max.y = 10.0f; wall.max.z = 9.6f;
    saige::Aabb low;   // a parked car: 1.5 m tall, 20 m away from the wall
    low.min.x = 30.0f; low.min.y = 0.0f; low.min.z = 0.0f;
    low.max.x = 34.0f; low.max.y = 2.0f; low.max.z = 1.5f;
    d.collision = {wall, low};
    using V3 = std::array<float, 3>;
    // eye_in_solid: inside the wall box yes; beside it no; above it no; inside a box lower than an eye no.
    CHECK(eye_in_solid(d, V3{5, 5, 1.7f}), "an eye inside the wall box is in solid");
    CHECK(!eye_in_solid(d, V3{-1, 5, 1.7f}), "an eye beside the box is not");
    CHECK(!eye_in_solid(d, V3{5, 5, 12.0f}), "an eye above the box is not");
    CHECK(!eye_in_solid(d, V3{32, 1, 1.0f}), "a box lower than an eye does not count");
    // view_into_solid: the wall's face at x = 0.
    CHECK(view_into_solid(d, V3{-1.0f, 5, 1.7f}, V3{5, 5, 1.7f}, 1.2f), "a wall 1 m ahead is within 1.2 m");
    CHECK(!view_into_solid(d, V3{-2.0f, 5, 1.7f}, V3{5, 5, 1.7f}, 1.2f), "a wall 2 m ahead is not within 1.2 m");
    CHECK(view_into_solid(d, V3{-2.0f, 5, 1.7f}, V3{5, 5, 1.7f}, 3.0f), "a wall 2 m ahead is within 3 m");
    CHECK(!view_into_solid(d, V3{-1.0f, 5, 1.7f}, V3{-8, 5, 1.7f}, 3.0f), "looking away from the wall");
    CHECK(!view_into_solid(d, V3{-1.0f, -3, 1.7f}, V3{-1.0f, 20, 1.7f}, 30.0f), "looking along the wall, 1 m from it, does not hit it");
    CHECK(view_into_solid(d, V3{-1.0f, -1, 1.7f}, V3{4, 4, 1.7f}, 3.0f), "a diagonal ray hits the corner region within 3 m");
    CHECK(!view_into_solid(d, V3{25.0f, 1, 1.0f}, V3{36, 1, 1.0f}, 20.0f), "a box lower than an eye is never a wall ahead");
    // sightline_blocked_by_building: segment [0, 0.9] to the subject against the building shrunk by 5 cm.
    CHECK(sightline_blocked_by_building(d, V3{-10, 5, 1.7f}, V3{20, 5, 1.0f}), "the building stands between the eye and the subject");
    CHECK(!sightline_blocked_by_building(d, V3{-10, 5, 1.7f}, V3{-2, 5, 1.0f}), "the subject is in front of the building");
    CHECK(!sightline_blocked_by_building(d, V3{-10, 20, 1.7f}, V3{20, 20, 1.0f}), "the line passes beside the building");
    CHECK(!sightline_blocked_by_building(d, V3{-10, 5, 15.0f}, V3{20, 5, 14.0f}), "the line passes over the building");
    CHECK(!sightline_blocked_by_building(d, V3{5, 5, 1.7f}, V3{20, 5, 1.0f}), "an eye inside the building is not judged by it");
    CHECK(!sightline_blocked_by_building(d, V3{-10, 5, 1.7f}, V3{-0.2f, 5, 1.2f}), "a subject standing against the wall is not hidden by it (the last tenth of the line is exempt)");
    CHECK(!sightline_blocked_by_building(d, V3{-10, 5, 1.7f}, V3{0.5f, 5, 1.2f}), "a subject 0.5 m inside the footprint's edge, seen from 10 m, is exempt for its last tenth of the line");
    CHECK(sightline_blocked_by_building(d, V3{-10, 5, 1.7f}, V3{3.0f, 5, 1.2f}), "a subject 3 m inside the building is hidden");
    CHECK(!sightline_blocked_by_building(d, V3{-10, 0.03f, 1.7f}, V3{20, 0.03f, 1.7f}), "a line grazing the wall 3 cm inside its face is inside the 5 cm shrink and does not count as blocked");
    CHECK(sightline_blocked_by_building(d, V3{-10, 0.2f, 1.7f}, V3{20, 0.2f, 1.7f}), "a line 20 cm inside the face passes through the building");
    // view_obstructed: a tree's canopy and an awning against nine rays.
    StreetProp tree;
    tree.type = "tree";
    tree.x = 50;
    tree.y = 0;
    tree.z = 0;
    tree.scale = 1.0;
    std::vector<StreetProp> trees{tree};
    const auto blobs = tree_blobs(50.0f, 0.0f, 0.0f, 0.0f, 1.0f, tree_seed(tree));
    const CanopyBlob c = blobs[0];
    const std::vector<Box3> no_awnings;
    CHECK(view_obstructed(trees, no_awnings, V3{c.x - c.r - 2.0f, c.y, c.z}, V3{c.x, c.y, c.z}, 55.0f), "leaves 2 m ahead of the lens obstruct the view");
    CHECK(!view_obstructed(trees, no_awnings, V3{c.x - c.r - 40.0f, c.y, c.z}, V3{c.x - c.r - 80.0f, c.y, c.z}, 55.0f), "looking away from a tree 40 m behind is not obstructed");
    CHECK(!view_obstructed(trees, no_awnings, V3{c.x, c.y + 60.0f, c.z}, V3{c.x, c.y + 120.0f, c.z}, 55.0f), "a tree 60 m behind the eye does not obstruct");
    const std::vector<Box3> awning{{-100.0f, 1.0f, 2.22f, 100.0f, 2.5f, 3.0f}};   // a wide awning 1 m in front of an eye at 2.05 m looking along +y
    CHECK(view_obstructed({}, awning, V3{0, 0, 2.05f}, V3{0, 20, 2.05f}, 55.0f), "an awning slab across the top of the frame obstructs");
    CHECK(!view_obstructed({}, awning, V3{0, 0, 1.2f}, V3{0, 20, 1.2f}, 55.0f), "a camera well below the awning's edge (1.2 m, the lowest ray stays under it) is not obstructed by it");
    CHECK(!view_obstructed({}, awning, V3{0, 0, 2.05f}, V3{0, -20, 2.05f}, 55.0f), "an awning behind the eye does not obstruct");
    const std::vector<Box3> tiny{{-0.1f, -0.1f, 1.9f, 0.1f, 0.1f, 2.1f}};   // a box around the eye that the first ray sample already leaves
    CHECK(view_obstructed({}, tiny, V3{0, 0, 2.0f}, V3{0, 20, 2.0f}, 55.0f), "an eye inside an awning box is obstructed even when its rays leave the box at once");
}

struct Case {
    std::uint32_t seed;
    bool real;
};

struct Result {
    int frames = 0, obstructed = 0, solid = 0, hidden = 0, bad = 0;
    int over_ceiling = 0;   // frames whose displacement from the proposal exceeds the budget (5 m horizontal, 2.5 m up) although the director did not count them as beyond the budget
    double max_h = 0, max_lift = 0, max_jump = 0;
    int pops = 0;
    int unflagged_jumps = 0, swept_blocked = 0, forced_cuts = 0, min_cut_gap = 1 << 30;
    double max_eye_step = 0, max_look_step_deg = 0;
    rend::ClearanceStats stats;
    std::string worst_jump, worst_look;
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
    rend::Cam prev_cam;
    bool have_prev = false;
    int last_forced_frame = -1;
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
            if (jump > 1.5 + 1e-3) ++r.pops;
            if (jump > r.max_jump) {
                r.max_jump = jump;
                r.worst_jump = "frame " + std::to_string(f) + " in '" + shot + "'";
            }
        }
        // Continuity between frames of one shot, measured on the final camera. A frame that is flagged as a cut (a shot start, or a forced jump of the clearance) is exempt from the continuity limits and is counted;
        // every other frame must move continuously: the offset may not change by more than 1.5 m (an unflagged jump is a failure), the eye's swept segment must not pass through a wall or tall prop, and the eye and
        // the view direction move by bounded amounts.
        if (have_prev && shot == prev_shot) {
            const double jump = std::sqrt(static_cast<double>((off[0] - prev_off[0]) * (off[0] - prev_off[0]) + (off[1] - prev_off[1]) * (off[1] - prev_off[1]) + (off[2] - prev_off[2]) * (off[2] - prev_off[2])));
            if (jump > 1.5 + 1e-3 && !cam.forced_cut) {
                ++r.unflagged_jumps;
                if (std::getenv("ALIVE_CAMERA_TEST_DEBUG") != nullptr) std::printf("  unflagged jump %.2f m at frame %d in '%s': offset (%.2f %.2f %.2f) -> (%.2f %.2f %.2f), cut %d\n", jump, f, shot.c_str(), static_cast<double>(prev_off[0]), static_cast<double>(prev_off[1]), static_cast<double>(prev_off[2]), static_cast<double>(off[0]), static_cast<double>(off[1]), static_cast<double>(off[2]), cam.cut ? 1 : 0);
            }
            if (cam.forced_cut) {
                ++r.forced_cuts;
                if (last_forced_frame >= 0) r.min_cut_gap = std::min(r.min_cut_gap, f - last_forced_frame);
                last_forced_frame = f;
            }
            if (!cam.cut) {
                const std::array<float, 3> a{prev_cam.eye.x, prev_cam.eye.y, prev_cam.eye.z};
                r.max_eye_step = std::max(r.max_eye_step, std::sqrt(static_cast<double>((eye[0] - a[0]) * (eye[0] - a[0]) + (eye[1] - a[1]) * (eye[1] - a[1]) + (eye[2] - a[2]) * (eye[2] - a[2]))));
                if (view_into_solid(d, a, eye, std::sqrt((eye[0] - a[0]) * (eye[0] - a[0]) + (eye[1] - a[1]) * (eye[1] - a[1]) + (eye[2] - a[2]) * (eye[2] - a[2])))) ++r.swept_blocked;
                const double d0[3] = {static_cast<double>(prev_cam.look.x - prev_cam.eye.x), static_cast<double>(prev_cam.look.y - prev_cam.eye.y), static_cast<double>(prev_cam.look.z - prev_cam.eye.z)};
                const double d1[3] = {static_cast<double>(look[0] - eye[0]), static_cast<double>(look[1] - eye[1]), static_cast<double>(look[2] - eye[2])};
                const double l0 = std::sqrt(d0[0] * d0[0] + d0[1] * d0[1] + d0[2] * d0[2]), l1 = std::sqrt(d1[0] * d1[0] + d1[1] * d1[1] + d1[2] * d1[2]);
                if (l0 > 1e-9 && l1 > 1e-9) {
                    const double cs = std::clamp((d0[0] * d1[0] + d0[1] * d1[1] + d0[2] * d1[2]) / (l0 * l1), -1.0, 1.0);
                    const double deg = std::acos(cs) * 180.0 / 3.14159265358979323846;
                    if (deg > r.max_look_step_deg) {
                        r.max_look_step_deg = deg;
                        r.worst_look = "frame " + std::to_string(f) + " in '" + shot + "'";
                    }
                }
            }
        }
        prev_cam = cam;
        have_prev = true;
        prev_shot = shot;
        for (int k = 0; k < 3; ++k) prev_off[k] = off[k];
    }
    r.stats = director.clearance_stats();
    r.over_ceiling = std::max(0, over_budget_seen - r.stats.over_budget);
    return r;
}


} // namespace

int main(int argc, char** argv) {
    analytic_cases();
    if (argc == 2 && std::string(argv[1]) == "--analytic") {   // only the hand-worked geometry cases (fast: the mutation runs of the predicates use this)
        if (failures == 0) std::printf("analytic camera cases passed\n");
        return failures == 0 ? 0 : 1;
    }
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
        std::printf("seed %2u %s: %d frames, bad %d (obstructed %d, solid %d, hidden %d), displaced %d, shift %.2f m horizontal %.2f m up, beyond budget %d, re-framed %d, unresolved %d, largest change of the shift %.2f m, %d jumps over 1.5 m (%s); forced cuts %d (closest %d frames apart), unflagged jumps %d, swept-through-wall steps %d, largest eye step %.2f m, largest view turn %.1f deg (%s)\n", cs.seed, cs.real ? "real " : "none ", r.frames, r.bad, r.obstructed, r.solid, r.hidden, r.stats.displaced_frames, r.max_h, r.max_lift, r.stats.over_budget, r.stats.reframed, r.stats.unresolved, r.max_jump, r.pops, r.worst_jump.c_str(), r.forced_cuts, r.min_cut_gap == (1 << 30) ? -1 : r.min_cut_gap, r.unflagged_jumps, r.swept_blocked, r.max_eye_step, r.max_look_step_deg, r.worst_look.c_str());
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
        // passing and the nearest passing one is far away the eye jumps (5.6 m on seed 4, in a follow shot of a resident). The test bounds it so that it cannot get worse unnoticed: a forced cut is at most two budgets wide (11.5 m); the largest observed is 8.2 m on seed 4 in one frame
        // and at most 3 jumps over 1.5 m in a film (measured: at most 2 per film and 5.6 m). The director's own count must agree with the test's.
        CHECK(r.max_jump <= 11.5, "seed %u %s: the clearance moved the eye by %.2f m in one frame (%s): more than two budgets apart", cs.seed, cs.real ? "real" : "none", r.max_jump, r.worst_jump.c_str());
        CHECK(r.forced_cuts == r.stats.pops && r.pops <= r.forced_cuts, "seed %u %s: %d jumps over 1.5 m in this film, the director counted %d and flagged %d as cuts", cs.seed, cs.real ? "real" : "none", r.pops, r.stats.pops, r.forced_cuts);
        // No silent jumps: every change of the offset above 1.5 m is flagged as a cut (Cam::forced_cut) and counted; the cuts are few and not clustered (a known limit: two cuts within half a second happen on seed 4); no frame that is not a cut passes through a wall.
        CHECK(r.unflagged_jumps == 0, "seed %u %s: %d unflagged jumps", cs.seed, cs.real ? "real" : "none", r.unflagged_jumps);
        CHECK(r.forced_cuts <= 3 && (r.forced_cuts < 2 || r.min_cut_gap >= 15), "seed %u %s: %d forced cuts, closest %d frames apart (at most 3, at least 15 frames apart: a KNOWN LIMIT, see the register)", cs.seed, cs.real ? "real" : "none", r.forced_cuts, r.min_cut_gap);
        CHECK(r.swept_blocked == 0, "seed %u %s: %d frames moved the eye through a wall or tall prop", cs.seed, cs.real ? "real" : "none", r.swept_blocked);
        CHECK(r.max_look_step_deg <= 30.0, "seed %u %s: the view turned by %.1f degrees in one frame outside a cut", cs.seed, cs.real ? "real" : "none", r.max_look_step_deg);
    }
    CHECK(walked > 0, "no case was walked");
    if (failures == 0) std::printf("camera tests passed (%d cases)\n", walked);
    return failures == 0 ? 0 : 1;
}
