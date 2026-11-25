// The animation against the physical character: speed from the capsule (not the input), feet that stay on ramps, feet that stay out of walls, and jump states that follow the capsule's own.
#include "saige/gfx_test/saige_test.hpp"

#include "fixtures.hpp"
#include "metrics.hpp"
#include "saige/animation/capsule_glue.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
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

namespace {

struct RollResult {
    float travelled = 0.0f;        // meters the capsule went along the heading
    float claimed = 0.0f;          // meters the clip says it travelled when the run ended
    float lateral = 0.0f;          // meters sideways of the heading
    float clip_time = 0.0f;
    bool stalled = false;
    int ticks = 0;
    float worst_swim = 0.0f;       // how far the pose's hips were from the origin in the horizontal plane
};

// Plays an action on the capsule from (x0, 0): each tick asks the action for its velocity, drives the capsule with it, measures how much of it the capsule covered and gives that back.
RollResult roll(const fixtures::Biped& b, const ActionClip& action, float x0, float yaw, bool feedback) {
    const Level level = make_level(0.0f);
    CapsuleCharacter character({x0, 0.0f, 0.0f}, level.boxes, level.triangles, CapsuleCharacterConfig{});
    ActionPlayer player;
    player.start(action);
    const gfx::Pose rest = gfx::rest_pose(b.skin);
    const std::size_t hips = static_cast<std::size_t>(b.map[Bone::Hips]);
    const Float3 rest_hips = joint_world(b.skin, rest)[hips].position;
    const float dt = 1.0f / 60.0f;
    RollResult r;
    const Vec3 start = character.state().feet;
    const float c = std::cos(yaw), s = std::sin(yaw);
    while (player.active() && r.ticks < 600) {
        CapsuleInput in;
        in.drive = capsule_drive(player, dt, yaw);
        const Vec3 before = character.state().feet;
        character.step(in, tick);
        const float progress = feedback ? drive_progress(before, character.state().feet, in.drive, dt) : 1.0f;
        player.advance(dt, progress);
        const Float3 h = joint_world(b.skin, player.pose())[hips].position;
        r.worst_swim = std::max(r.worst_swim, std::hypot(h.x - rest_hips.x, h.y - rest_hips.y));
        ++r.ticks;
    }
    const Vec3 end = character.state().feet;
    r.travelled = (end.x - start.x) * c + (end.y - start.y) * s;
    r.lateral = -(end.x - start.x) * s + (end.y - start.y) * c;
    r.claimed = action.offset(player.time()).forward;
    r.clip_time = player.time();
    r.stalled = player.stalled();
    return r;
}

} // namespace

TEST_CASE("Action on the capsule: a roll on open ground covers the clip's travel, along any heading, with the body in place") {
    const fixtures::Biped b = fixtures::make_biped();
    const ActionClip action(b.skin, b.map, b.rig, fixtures::make_root_motion_clip(b, 1.0f, 4.0f, 0.0f, 0.3f));
    for (const float yaw : {0.0f, 0.7f, 1.5707963f, -2.4f}) {
        const RollResult r = roll(b, action, 0.0f, yaw, true);
        CHECK(std::fabs(r.travelled - 4.0f) < 0.03f);   // the capsule went as far as the clip's hips did
        CHECK(std::fabs(r.lateral) < 0.02f);            // along the heading, not beside it
        CHECK(!r.stalled);
        CHECK(r.worst_swim < 2e-3f);                    // and the pose stayed at the capsule
        CHECK(std::fabs(r.claimed - 4.0f) < 0.01f);     // the whole clip played
    }
}

TEST_CASE("Action on the capsule: a roll into a wall stops at the wall, follows what the physics allowed, and gives up; ignoring the physics plays on through the wall") {
    const fixtures::Biped b = fixtures::make_biped();
    const ActionClip action(b.skin, b.map, b.rig, fixtures::make_root_motion_clip(b, 1.0f, 4.0f, 0.0f, 0.3f));
    const float x0 = 40.0f - 0.3f - 1.5f;  // 1.5 m of room before the wall's face at x = 40 (the capsule's radius is 0.3)
    const RollResult with = roll(b, action, x0, 0.0f, true);
    const RollResult without = roll(b, action, x0, 0.0f, false);
    std::printf("  with feedback: capsule %.2f m, clip claims %.2f m, clip time %.2f s, stalled %d, %d ticks; without: capsule %.2f m, clip claims %.2f m, clip time %.2f s\n", static_cast<double>(with.travelled),
                static_cast<double>(with.claimed), static_cast<double>(with.clip_time), with.stalled ? 1 : 0, with.ticks, static_cast<double>(without.travelled), static_cast<double>(without.claimed),
                static_cast<double>(without.clip_time));
    // The capsule is stopped by the wall either way.
    CHECK(with.travelled < 1.55f);
    CHECK(without.travelled < 1.55f);
    // With the feedback the clip says what the capsule did (to a few centimeters), stops there and ends.
    CHECK(std::fabs(with.claimed - with.travelled) < 0.06f);
    CHECK(with.stalled);
    CHECK(with.clip_time < 0.7f);
    // Without it the animation claims the full roll (a body that rolled four meters through a wall) while the capsule is 1.5 m along: that is the defect the feedback removes.
    CHECK(without.claimed > 3.9f);
    CHECK(without.claimed - without.travelled > 2.0f);
    CHECK(!without.stalled);
}

TEST_CASE("Action on the capsule: a roll off a ledge is ended by the host the moment the capsule leaves the ground, and the pose follows") {
    const fixtures::Biped b = fixtures::make_biped();
    const ActionClip action(b.skin, b.map, b.rig, fixtures::make_root_motion_clip(b, 1.0f, 4.0f, 0.0f, 0.3f));
    // A platform 1 m above the floor that ends at x = 2: rolling off it leaves the ground.
    std::vector<Aabb> boxes{{{-50, -50, -1}, {2, 50, 1.0f}}, {{2, -50, -1}, {100, 50, -0.5f}}};
    const StaticCollision world(boxes, {});
    CapsuleCharacter character({0.0f, 0.0f, 1.0f}, boxes, CapsuleCharacterConfig{});
    ActionPlayer player;
    player.start(action);
    const float dt = 1.0f / 60.0f;
    int left_ground_at = -1, tick_count = 0;
    while (player.active() && tick_count < 300) {
        CapsuleInput in;
        in.drive = capsule_drive(player, dt, 0.0f);
        const Vec3 before = character.state().feet;
        character.step(in, tick);
        if (!character.state().grounded && left_ground_at < 0) {
            left_ground_at = tick_count;
            player.cancel();  // the host's rule: an action does not outlive the ground under it
        }
        player.advance(dt, drive_progress(before, character.state().feet, in.drive, dt));
        ++tick_count;
    }
    CHECK(left_ground_at > 0);
    CHECK(!player.active());
    CHECK(!player.stalled());
    CHECK(character.state().feet.x > 1.95f);
}

namespace {

struct ClimbResult {
    Vec3 end;
    bool grounded = false;
    bool stalled = false;
    float worst_pose_rise = 0.0f;  // how far the pose's hips were above their start (the climb's rise is the capsule's, not the pose's)
    int ticks = 0;
};

// Climbs with a vertical action on a 1 m-ish ledge whose face is at x = 1.2 and whose height is `ledge`.
ClimbResult climb(const fixtures::Biped& b, const ActionClip& action, float ledge, bool feedback, bool ceiling = false) {
    std::vector<Aabb> boxes{{{-50, -20, -1}, {50, 20, 0}}, {{1.2f, -20, 0}, {50, 20, ledge}}};
    if (ceiling) boxes.push_back({{-5, -20, 2.0f}, {50, 20, 3.0f}});  // a ceiling over the whole run, 2 m up: the capsule (1.7 m) can rise 0.3 m, and the way forward is open
    CapsuleCharacter character({0.0f, 0.0f, 0.0f}, boxes, CapsuleCharacterConfig{});
    for (int i = 0; i < 6; ++i) character.step({}, tick);
    ActionPlayer player;
    player.start(action);
    const std::size_t hips = static_cast<std::size_t>(b.map[Bone::Hips]);
    const float start_z = joint_world(b.skin, player.pose())[hips].position.z;
    const float dt = 1.0f / 60.0f;
    ClimbResult r;
    while (player.active() && r.ticks < 600) {
        CapsuleInput in;
        in.drive = capsule_drive(player, dt, 0.0f);
        const Vec3 before = character.state().feet;
        character.step(in, tick);
        player.advance(dt, feedback ? drive_progress(before, character.state().feet, in.drive, dt) : 1.0f);
        r.worst_pose_rise = std::max(r.worst_pose_rise, joint_world(b.skin, player.pose())[hips].position.z - start_z);
        ++r.ticks;
    }
    for (int i = 0; i < 30; ++i) character.step({}, tick);  // let it settle
    r.end = character.state().feet;
    r.grounded = character.state().grounded;
    r.stalled = player.stalled();
    return r;
}

} // namespace

TEST_CASE("Vertical action on the capsule: a climb carries the character up onto a ledge and the pose stays at the capsule; a ledge too high for the clip stops it without going through") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::AnimationClip clip = fixtures::make_climb_clip(b, 1.0f, 1.05f, 2.0f);
    const ActionClip action(b.skin, b.map, b.rig, clip, 60.0f, true);
    REQUIRE(action.valid());
    CHECK(action.vertical());
    CHECK(std::fabs(action.travel().forward - 2.0f) < 5e-3f);
    CHECK(std::fabs(action.travel().up - 1.0f) < 5e-3f);       // 1.05 up, 0.05 settled
    CHECK(action.travel_length() > 2.2f);
    // Without the vertical option the rise is not root motion.
    const ActionClip flat(b.skin, b.map, b.rig, clip);
    CHECK(flat.travel().up == 0.0f);
    const ClimbResult ok = climb(b, action, 1.0f, true);
    std::printf("  climb onto a 1.0 m ledge: feet at (%.2f, %.2f), grounded %d, stalled %d, %d ticks, pose rose %.1f mm\n", static_cast<double>(ok.end.x), static_cast<double>(ok.end.z), ok.grounded ? 1 : 0, ok.stalled ? 1 : 0,
                ok.ticks, static_cast<double>(ok.worst_pose_rise) * 1000.0);
    CHECK(ok.grounded);
    CHECK(std::fabs(ok.end.z - 1.0f) < 0.02f);
    CHECK(std::fabs(ok.end.x - 2.0f) < 0.1f);
    CHECK(!ok.stalled);
    CHECK(ok.worst_pose_rise < 2e-3f);
    // A 1.6 m wall: the clip cannot carry the character over it. It rises along the face, stalls, and never ends up on or beyond it.
    const ClimbResult wall = climb(b, action, 1.6f, true);
    std::printf("  climb at a 1.6 m wall: feet at (%.2f, %.2f), stalled %d\n", static_cast<double>(wall.end.x), static_cast<double>(wall.end.z), wall.stalled ? 1 : 0);
    CHECK(wall.end.x < 1.2f - 0.29f);
    CHECK(wall.end.z < 0.3f || wall.grounded);   // back on the floor after the action gave up (gravity), not on the wall
    CHECK(wall.stalled);
    // A ceiling 2 m up over the whole run, with only a 5 cm step ahead: the rise is blocked while the way forward is open, so only the 3D progress sees it; the action gives up and the character stays under the ceiling.
    const ClimbResult low = climb(b, action, 0.05f, true, true);
    const ClimbResult blind = climb(b, action, 0.05f, false, true);
    std::printf("  climb under a low ceiling: feet at (%.2f, %.2f) stalled %d; ignoring the physics the clip plays on (%d ticks)\n", static_cast<double>(low.end.x), static_cast<double>(low.end.z), low.stalled ? 1 : 0, blind.ticks);
    CHECK(low.stalled);
    CHECK(low.end.z < 0.3f);
    CHECK(!blind.stalled);
}

// The real climb clip (UAL 2's ClimbUp_1m_RM) on the capsule: needs the optional asset (SAIGE_ANIM_ASSETS/ual2/UAL2_Standard.glb, see tools/animation/fetch_humanoid_assets.py); reports and passes without it.
TEST_CASE("Vertical action on the capsule: UAL 2's ClimbUp_1m_RM climbs a 1 m ledge, in place on the rig, with the capsule doing the travel") {
    const char* env = std::getenv("SAIGE_ANIM_ASSETS");
    const std::filesystem::path path = env != nullptr ? std::filesystem::path(env) / "ual2" / "UAL2_Standard.glb" : std::filesystem::path();
    if (env == nullptr || !std::filesystem::exists(path)) {
        std::printf("  UAL 2 not available (SAIGE_ANIM_ASSETS/ual2/UAL2_Standard.glb): real climb skipped\n");
        return;
    }
    const gfx::GltfLoadResult loaded = gfx::load_gltf_file(path);
    REQUIRE(loaded.ok());
    const gfx::Skin& skin = loaded.asset->skins[0];
    const auto map = map_humanoid(skin);
    REQUIRE(map.has_value());
    const RigInfo rig = analyze_rig(skin, *map);
    const gfx::AnimationClip* clip = nullptr;
    for (const gfx::AnimationClip& c : loaded.asset->animations) {
        if (c.name == "ClimbUp_1m_RM") clip = &c;
    }
    REQUIRE(clip != nullptr);
    const ActionClip action(skin, *map, rig, *clip, 60.0f, true);
    std::printf("  ClimbUp_1m_RM: %.2f s, travel %.2f m forward, %.2f m up\n", static_cast<double>(action.duration()), static_cast<double>(action.travel().forward), static_cast<double>(action.travel().up));
    CHECK(std::fabs(action.travel().up - 1.0f) < 0.1f);
    CHECK(action.travel().forward > 1.4f && action.travel().forward < 2.0f);
    // Where the clip puts its first foot on the ledge: the toes' forward position (relative to the hips' start) when they first reach the ledge's height.
    const gfx::ClipBinding bind = gfx::bind_clip(*clip, skin);
    gfx::Pose pose = gfx::rest_pose(skin);
    gfx::evaluate_clip(*clip, bind, 0.0f, false, pose);
    const Float3 start = joint_world(skin, pose)[static_cast<std::size_t>((*map)[Bone::Hips])].position;
    const float floor_z = std::min(joint_world(skin, pose)[static_cast<std::size_t>((*map)[Bone::LeftToes])].position.z, joint_world(skin, pose)[static_cast<std::size_t>((*map)[Bone::RightToes])].position.z);
    float face = 0.0f;
    for (int i = 0; i <= 200 && face == 0.0f; ++i) {
        gfx::evaluate_clip(*clip, bind, clip->duration * static_cast<float>(i) / 200.0f, false, pose);
        const std::vector<JointWorld> w = joint_world(skin, pose);
        for (const Bone toes : {Bone::LeftToes, Bone::RightToes}) {
            const Float3 t = w[static_cast<std::size_t>((*map)[toes])].position;
            if (t.z - floor_z > 0.9f && face == 0.0f) face = t.x - start.x;
        }
    }
    std::printf("  the first toe reaches ledge height %.2f m ahead of the hips' start\n", static_cast<double>(face));
    REQUIRE(face > 0.3f);
    // A ledge 1 m high whose face is where the clip's first foot lands, less the length of the foot; the character stands at the start.
    const float face_x = face - 0.12f;
    std::vector<Aabb> boxes{{{-50, -20, -1}, {50, 20, 0}}, {{face_x, -20, 0}, {50, 20, 1.0f}}};
    CapsuleCharacter character({0.0f, 0.0f, 0.0f}, boxes, CapsuleCharacterConfig{});
    for (int i = 0; i < 6; ++i) character.step({}, tick);
    ActionPlayer player;
    player.start(action);
    const float dt = 1.0f / 60.0f;
    int ticks = 0;
    float worst_swim = 0.0f;
    while (player.active() && ticks < 600) {
        CapsuleInput in;
        in.drive = capsule_drive(player, dt, 0.0f);
        const Vec3 before = character.state().feet;
        character.step(in, tick);
        player.advance(dt, drive_progress(before, character.state().feet, in.drive, dt));
        const Float3 h = joint_world(skin, player.pose())[static_cast<std::size_t>((*map)[Bone::Hips])].position;
        worst_swim = std::max(worst_swim, std::hypot(h.x - start.x, h.y - start.y));
        worst_swim = std::max(worst_swim, std::fabs(h.z - start.z));
        ++ticks;
    }
    for (int i = 0; i < 30; ++i) character.step({}, tick);
    const CapsuleCharacterState& s = character.state();
    std::printf("  climbed: feet at (%.2f, %.2f), grounded %d, stalled %d, %d ticks, pose %.1f mm from its start\n", static_cast<double>(s.feet.x), static_cast<double>(s.feet.z), s.grounded ? 1 : 0, player.stalled() ? 1 : 0,
                ticks, static_cast<double>(worst_swim) * 1000.0);
    CHECK(s.grounded);
    CHECK(std::fabs(s.feet.z - 1.0f) < 0.03f);
    CHECK(s.feet.x > face_x);
    CHECK(!player.stalled());
    // The pose is in place in all three axes: the capsule did all the travel.
    CHECK(worst_swim < 0.005f);
}
