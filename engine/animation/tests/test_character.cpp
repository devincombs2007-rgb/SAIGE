// The animation against the physical character: speed from the capsule (not the input), feet that stay on ramps, feet that stay out of walls, and jump states that follow the capsule's own.
#include "saige/gfx_test/saige_test.hpp"

#include "fixtures.hpp"
#include "metrics.hpp"
#include "saige/animation/capsule_glue.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

using namespace saige;
using namespace saige::anim;
using render::Float3;

namespace {

struct Level {
    std::vector<Aabb> boxes;
    std::vector<Triangle> triangles;
    StaticCollision world;
};

// A floor, a ramp rising `degrees` from x = 10 over 8 m to a plateau, and a wall at x = 40.
Level make_level(float degrees) {
    Level l;
    l.boxes.push_back({{-100, -50, -1}, {200, 50, 0}});            // the floor, top at z = 0
    const float rise = 8.0f * std::tan(degrees * 3.14159265f / 180.0f);
    l.boxes.push_back({{18, -50, -1}, {200, 50, rise}});           // the plateau after the ramp
    l.boxes.push_back({{40, -50, -1}, {41, 50, 20}});              // a wall
    // The ramp: two triangles from (10, +-50, 0) to (18, +-50, rise).
    const Vec3 a{10, -50, 0}, b{10, 50, 0}, c{18, 50, rise}, d{18, -50, rise};
    l.triangles.push_back({a, b, c});
    l.triangles.push_back({a, c, d});
    l.world = StaticCollision(l.boxes, l.triangles);
    return l;
}

struct Rig {
    fixtures::Biped biped = fixtures::make_biped();
    LocomotionSet set;
    Rig() : set(biped.skin, biped.map) {
        CHECK(set.add_cycle(fixtures::make_gait(biped, 1.2f, 1.10f, 0.60f, 0.08f)));
        CHECK(set.add_cycle(fixtures::make_gait(biped, 3.5f, 0.70f, 0.38f, 0.20f)));
        CHECK(set.add_cycle(fixtures::make_gait(biped, 7.0f, 0.55f, 0.28f, 0.30f)));
        set.set_idle(fixtures::make_pose_clip(biped, 2.0f, 0.0f, 0.0f));
        set.set_jump_start(fixtures::make_pose_clip(biped, 0.6f, 0.15f, 0.5f));
        set.set_air(fixtures::make_pose_clip(biped, 1.0f, 0.0f, 0.9f));
        set.set_land(fixtures::make_pose_clip(biped, 0.5f, 0.25f, 0.2f));
    }
};

float yaw_x() { return 0.0f; }

constexpr std::chrono::nanoseconds tick{16666667};

} // namespace

TEST_CASE("Character animation: a character that runs into a wall stops, its feet stay out of the wall, and it does not run on the spot") {
    Rig w;
    const Level level = make_level(0.0f);  // flat
    CapsuleCharacterConfig cfg;
    cfg.radius = 0.30f;
    for (const bool follow_physics : {true, false}) {
        for (const bool wall_ik : {true, false}) {
            CapsuleCharacter character({30.0f, 0.0f, 0.0f}, level.boxes, level.triangles, cfg);
            LocomotionController anim(w.set, {});
            const ObstacleQuery obstacle = obstacle_query(level.world);
            float deepest = -1e9f;
            double stuck_slide = 0.0;
            int stuck_samples = 0;
            Float3 prev_toe[2];
            bool have = false;
            for (int i = 0; i < 360; ++i) {
                character.step({1.0f, 0.0f, false, true}, tick);  // sprint straight at the wall, the whole time
                const CapsuleCharacterState& s = character.state();
                LocomotionInput in = locomotion_input(s);
                if (!follow_physics) in.speed = cfg.sprint_speed;  // the control: animate from the input, as if the wall were not there
                anim.update(1.0f / 60.0f, in);
                gfx::Pose pose = anim.pose();
                const render::Transform root{{s.feet.x, s.feet.y, s.feet.z}, render::from_axis_angle({0, 0, 1}, yaw_x()), {1, 1, 1}};
                if (wall_ik) keep_feet_outside_obstacles(w.biped.skin, w.biped.map, pose, root, obstacle, {});
                const std::vector<JointWorld> jw = joint_world(w.biped.skin, pose);
                Float3 toe[2];
                float tip = -1e9f;
                for (int f = 0; f < 2; ++f) {
                    const Bone b = f == 0 ? Bone::LeftToes : Bone::RightToes;
                    toe[f] = root.position + jw[static_cast<std::size_t>(w.biped.map[b])].position;
                    tip = std::max(tip, toe[f].x);
                }
                deepest = std::max(deepest, tip - 40.0f);  // the wall's face is x = 40
                if (i > 240 && have) {  // the character has been against the wall for a while
                    for (int f = 0; f < 2; ++f) {
                        if (prev_toe[f].z < 0.01f && toe[f].z < 0.01f) {
                            stuck_slide += static_cast<double>(std::hypot(toe[f].x - prev_toe[f].x, toe[f].y - prev_toe[f].y)) * 60.0;
                            ++stuck_samples;
                        }
                    }
                }
                prev_toe[0] = toe[0];
                prev_toe[1] = toe[1];
                have = true;
            }
            const CapsuleCharacterState& s = character.state();
            CHECK(s.feet.x > 39.6f && s.feet.x < 39.75f);  // the capsule is against the wall (radius 0.3)
            const double mean_stuck = stuck_samples > 0 ? stuck_slide / stuck_samples : 0.0;
            if (follow_physics) {
                CHECK(anim.idle_weight() > 0.95f);        // standing, not running on the spot
                CHECK(mean_stuck < 0.05);
            } else {
                CHECK(anim.idle_weight() < 0.05f);        // the control keeps running against the wall
            }
            if (wall_ik) CHECK(deepest < 0.005f);          // no foot pokes into the wall
            else if (!follow_physics) CHECK(deepest > 0.05f);  // the control: without the wall IK, a running pose leans its feet into it
        }
    }
}

TEST_CASE("Character animation: on a ramp the planted feet stay on the surface with ground IK, and do not without it") {
    Rig w;
    for (const float degrees : {10.0f, 20.0f}) {
        const Level level = make_level(degrees);
        CapsuleCharacterConfig cfg;
        for (const bool use_ik : {true, false}) {
            CapsuleCharacter character({4.0f, 0.0f, 0.0f}, level.boxes, level.triangles, cfg);
            LocomotionController anim(w.set, {});
            FootIkState ik_state;
            const GroundQuery ground = ground_query(level.world);
            float worst = 0.0f;
            int planted = 0;
            for (int i = 0; i < 600; ++i) {
                character.step({1.0f, 0.0f, false, false}, tick);
                const CapsuleCharacterState& s = character.state();
                anim.update(1.0f / 60.0f, locomotion_input(s));
                gfx::Pose pose = anim.pose();
                const render::Transform root{{s.feet.x, s.feet.y, s.feet.z}, render::from_axis_angle({0, 0, 1}, yaw_x()), {1, 1, 1}};
                if (use_ik) adapt_feet_to_ground(w.biped.skin, w.biped.map, w.biped.rig, pose, root, ground, {}, ik_state, 1.0f / 60.0f);
                if (s.feet.x < 9.0f || s.feet.x > 17.0f || i < 60) continue;  // on the ramp
                const std::vector<JointWorld> jw = joint_world(w.biped.skin, pose);
                // The animation's own intent for a foot: planted when its contact point is on the character's own floor.
                const std::vector<JointWorld> raw = joint_world(w.biped.skin, anim.pose());
                for (const Bone toe : {Bone::LeftToes, Bone::RightToes}) {
                    const std::size_t j = static_cast<std::size_t>(w.biped.map[toe]);
                    if (raw[j].position.z > 0.004f) continue;  // in the air
                    const Float3 p = root.position + jw[j].position;
                    GroundSample g;
                    if (!ground(p.x, p.y, root.position.z, g)) continue;
                    worst = std::max(worst, std::fabs(p.z - g.height));
                    ++planted;
                }
            }
            CHECK(planted > 20);
            // With ground IK the planted feet are within a centimeter or two of the surface (the pelvis follows the ground a little behind a walking foot: more on a steeper slope).
            if (use_ik) CHECK(worst < (degrees < 15.0f ? 0.015f : 0.03f));
            else CHECK(worst > 0.06f);  // the control: a flat-ground animation on a slope floats or sinks
        }
    }
}

TEST_CASE("Character animation: the jump states follow the capsule's own takeoff, flight and landing") {
    Rig w;
    const Level level = make_level(10.0f);
    CapsuleCharacter character({4.0f, 0.0f, 0.0f}, level.boxes, level.triangles, {});
    LocomotionController anim(w.set, {});
    std::vector<LocomotionState> seen;
    int takeoff_tick = -1, landing_tick = -1, air_tick = -1, land_state_tick = -1;
    bool was_grounded = true;
    for (int i = 0; i < 300; ++i) {
        const bool jump = i == 60;
        character.step({1.0f, 0.0f, jump, false}, tick);
        const CapsuleCharacterState& s = character.state();
        anim.update(1.0f / 60.0f, locomotion_input(s));
        if (seen.empty() || seen.back() != anim.state()) seen.push_back(anim.state());
        if (was_grounded && !s.grounded && takeoff_tick < 0) takeoff_tick = i;
        if (!was_grounded && s.grounded && landing_tick < 0) landing_tick = i;
        if (anim.state() == LocomotionState::JumpStart && air_tick < 0) air_tick = i;
        if (anim.state() == LocomotionState::Land && land_state_tick < 0) land_state_tick = i;
        was_grounded = s.grounded;
    }
    REQUIRE(takeoff_tick > 0);
    REQUIRE(landing_tick > takeoff_tick);
    const std::vector<LocomotionState> want = {LocomotionState::Ground, LocomotionState::JumpStart, LocomotionState::Air, LocomotionState::Land, LocomotionState::Ground};
    CHECK(seen == want);
    CHECK(air_tick >= takeoff_tick && air_tick <= takeoff_tick + 2);          // the takeoff clip starts with the capsule's
    CHECK(land_state_tick >= landing_tick && land_state_tick <= landing_tick + 2);  // the landing starts with the capsule's
}
