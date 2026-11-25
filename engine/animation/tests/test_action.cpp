// Root-motion actions: the clip's travel is measured, commanded to the physics step by step, and followed back; the pose is the clip in place.
#include "saige/gfx_test/saige_test.hpp"

#include "fixtures.hpp"
#include "saige/animation/action.hpp"

#include <cmath>
#include <stdexcept>

using namespace saige;
using namespace saige::anim;
using render::Float3;

namespace {

float hips_xy_distance(const fixtures::Biped& b, const gfx::Pose& pose, const gfx::Pose& rest) {
    const std::size_t h = static_cast<std::size_t>(b.map[Bone::Hips]);
    const Float3 a = joint_world(b.skin, pose)[h].position, r = joint_world(b.skin, rest)[h].position;
    return std::hypot(a.x - r.x, a.y - r.y);
}

template <class F>
bool throws(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

float smooth(float u) { return u * u * (3.0f - 2.0f * u); }

} // namespace

TEST_CASE("Action clip: the hips' travel is measured along the character's frame and follows the clip's own (uneven) speed") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::AnimationClip clip = fixtures::make_root_motion_clip(b, 1.2f, 3.0f, 0.6f);
    const ActionClip action(b.skin, b.map, b.rig, clip);
    REQUIRE(action.valid());
    CHECK(std::fabs(action.duration() - 1.2f) < 1e-5f);
    CHECK(std::fabs(action.travel().forward - 3.0f) < 2e-3f);
    CHECK(std::fabs(action.travel().left - 0.6f) < 2e-3f);
    for (const float u : {0.0f, 0.1f, 0.25f, 0.5f, 0.8f, 1.0f}) {
        const PlanarVelocity o = action.offset(1.2f * u);
        CHECK(std::fabs(o.forward - 3.0f * smooth(u)) < 4e-3f);
        CHECK(std::fabs(o.left - 0.6f * smooth(u)) < 2e-3f);
    }
    // Outside the clip it clamps; a non-finite time is the start.
    CHECK(std::fabs(action.offset(-1.0f).forward) < 1e-6f);
    CHECK(std::fabs(action.offset(99.0f).forward - action.travel().forward) < 1e-6f);
    CHECK(action.offset(std::nanf("")).forward == 0.0f);
    // A clip with no duration is not an action.
    const ActionClip empty(b.skin, b.map, b.rig, gfx::AnimationClip{});
    CHECK(!empty.valid());
    ActionPlayer player;
    CHECK(throws([&] { player.start(empty); }));
}

TEST_CASE("Action player: the commanded velocities add up to the clip's travel, and the pose stays in place with the vertical motion kept") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::AnimationClip clip = fixtures::make_root_motion_clip(b, 1.2f, 3.0f, 0.0f, 0.25f);
    const ActionClip action(b.skin, b.map, b.rig, clip);
    const gfx::Pose rest = gfx::rest_pose(b.skin);
    const std::size_t hips = static_cast<std::size_t>(b.map[Bone::Hips]);
    const float rest_z = joint_world(b.skin, rest)[hips].position.z;
    ActionPlayer player;
    player.start(action);
    REQUIRE(player.active());
    const float dt = 1.0f / 60.0f;
    float travelled = 0.0f, worst_swim = 0.0f, lowest = 10.0f;
    int steps = 0;
    while (player.active() && steps < 600) {
        const PlanarVelocity v = player.command(dt);
        travelled += v.forward * dt;
        CHECK(std::fabs(v.left) < 1e-4f);
        player.advance(dt, 1.0f);
        worst_swim = std::max(worst_swim, hips_xy_distance(b, player.pose(), rest));
        lowest = std::min(lowest, joint_world(b.skin, player.pose())[hips].position.z);
        ++steps;
    }
    CHECK(!player.active());
    CHECK(!player.stalled());
    CHECK(std::fabs(travelled - 3.0f) < 3e-3f);
    CHECK(worst_swim < 2e-3f);                 // the body never leaves the origin: the position is the physics'
    CHECK(rest_z - lowest > 0.22f);            // and the dip of the roll is still there
    CHECK(steps >= 71 && steps <= 76);         // 1.2 s at 60 Hz, plus the fade out
    // Speeds are far from constant, so the control that removes only a linear drift (what a cycle gets) swims by decimeters.
    const ClipInfo info = analyze_cycle(b.skin, b.map, b.rig, clip);
    gfx::Pose linear = rest;
    float swim_linear = 0.0f;
    for (int i = 0; i < 72; ++i) {
        evaluate_in_place(b.skin, b.map, b.rig, clip, action.binding(), 1.2f * static_cast<float>(i) / 72.0f, info, linear);
        swim_linear = std::max(swim_linear, hips_xy_distance(b, linear, rest));
    }
    CHECK(swim_linear > 0.15f);
}

TEST_CASE("Action player: progress from the physics slows the clip, the commanded travel stays consistent, and an action that makes no headway ends") {
    const fixtures::Biped b = fixtures::make_biped();
    const ActionClip action(b.skin, b.map, b.rig, fixtures::make_root_motion_clip(b, 1.0f, 2.0f));
    const float dt = 1.0f / 60.0f;
    // Half progress takes twice as long and still delivers the clip's travel (the physics gave half of each command).
    ActionPlayer half;
    half.start(action);
    float delivered = 0.0f;
    int steps = 0;
    while (half.active() && steps < 1000) {
        delivered += half.command(dt).forward * dt * 0.5f;
        half.advance(dt, 0.5f);
        ++steps;
    }
    CHECK(std::fabs(delivered - 2.0f) < 4e-3f);
    CHECK(steps > 118 && steps < 130);
    // No progress at all: the clip holds where it is, and after stall_seconds the action fades out and is over, flagged as stalled.
    ActionPlayer stuck;
    stuck.start(action);
    for (int i = 0; i < 20; ++i) stuck.advance(dt, 1.0f);
    const float held = stuck.time();
    int stuck_steps = 0;
    while (stuck.active() && stuck_steps < 600) {
        stuck.advance(dt, 0.0f);
        ++stuck_steps;
    }
    CHECK(stuck.stalled());
    CHECK(!stuck.active());
    CHECK(std::fabs(stuck.time() - held) < 1e-6f);
    CHECK(stuck_steps > static_cast<int>(0.4f / dt) - 2);
    CHECK(stuck_steps < static_cast<int>((0.4f + 0.15f) / dt) + 6);
    CHECK(stuck.command(dt).forward == 0.0f);
    // A step with some progress resets the stall timer: alternating good and bad steps never stalls.
    ActionPlayer flicker;
    flicker.start(action);
    for (int i = 0; i < 40; ++i) flicker.advance(dt, i % 2 == 0 ? 0.0f : 1.0f);
    CHECK(!flicker.stalled());
    // Non-finite progress counts as none; a bad dt throws.
    ActionPlayer odd;
    odd.start(action);
    odd.advance(dt, std::nanf(""));
    CHECK(odd.time() == 0.0f);
    CHECK(throws([&] { odd.advance(0.0f, 1.0f); }));
    CHECK(throws([&] { odd.advance(std::nanf(""), 1.0f); }));
}

TEST_CASE("Action player: weight fades in and out, a cancel stops the travel and fades out, and the same inputs give the same pose") {
    const fixtures::Biped b = fixtures::make_biped();
    const ActionClip action(b.skin, b.map, b.rig, fixtures::make_root_motion_clip(b, 1.0f, 2.0f));
    ActionConfig cfg;
    cfg.fade_in = 0.1f;
    cfg.fade_out = 0.2f;
    const float dt = 1.0f / 60.0f;
    ActionPlayer a(cfg), twin(cfg);
    a.start(action);
    twin.start(action);
    CHECK(a.weight() == 0.0f);
    float previous = 0.0f;
    bool monotone_in = true;
    for (int i = 0; i < 6; ++i) {
        a.advance(dt, 1.0f);
        twin.advance(dt, 1.0f);
        monotone_in = monotone_in && a.weight() >= previous;
        previous = a.weight();
    }
    CHECK(monotone_in);
    CHECK(std::fabs(a.weight() - 1.0f) < 1e-5f);
    CHECK(pose_distance(a.pose(), twin.pose()) == 0.0f);
    // The last 0.2 s fade out.
    for (int i = 0; i < 36; ++i) {
        a.advance(dt, 1.0f);
        twin.advance(dt, 1.0f);
    }
    CHECK(a.weight() > 0.99f);
    while (a.active() && a.time() < 0.9f) a.advance(dt, 1.0f);
    CHECK(a.weight() < 0.55f);
    CHECK(a.weight() > 0.0f);
    // Cancel: no more travel, fades out over fade_out.
    ActionPlayer c(cfg);
    c.start(action);
    for (int i = 0; i < 12; ++i) c.advance(dt, 1.0f);
    CHECK(c.travelling());
    CHECK(std::fabs(c.command(dt).forward) > 0.0f);
    c.cancel();
    CHECK(!c.travelling());
    CHECK(c.command(dt).forward == 0.0f);
    int n = 0;
    while (c.active() && n < 100) {
        c.advance(dt, 1.0f);
        ++n;
    }
    CHECK(n >= 11 && n <= 14);
    CHECK(!c.stalled());
    CHECK(c.weight() == 0.0f);
}
