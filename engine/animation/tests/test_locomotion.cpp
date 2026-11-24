// Clip analysis, the locomotion state machine, stride-matched playback, transitions and foot sliding, on synthetic gaits whose speed, stride and foot phase are known exactly.
#include "saige/gfx_test/saige_test.hpp"

#include "fixtures.hpp"
#include "metrics.hpp"

using namespace saige;
using namespace saige::anim;
using render::Float3;

namespace {

struct World {
    fixtures::Biped biped = fixtures::make_biped();
    gfx::AnimationClip walk = fixtures::make_gait(biped, 1.2f, 1.10f, 0.60f, 0.08f);
    gfx::AnimationClip run = fixtures::make_gait(biped, 3.5f, 0.70f, 0.38f, 0.20f);
    gfx::AnimationClip sprint = fixtures::make_gait(biped, 7.0f, 0.55f, 0.28f, 0.30f);
    gfx::AnimationClip idle = fixtures::make_pose_clip(biped, 2.0f, 0.0f, 0.0f);
    gfx::AnimationClip jump_start = fixtures::make_pose_clip(biped, 0.6f, 0.15f, 0.5f);
    gfx::AnimationClip air = fixtures::make_pose_clip(biped, 1.0f, 0.0f, 0.9f);
    gfx::AnimationClip land = fixtures::make_pose_clip(biped, 0.5f, 0.25f, 0.2f);
    LocomotionSet set;
    World() : set(biped.skin, biped.map) {
        std::string why;
        CHECK(set.add_cycle(walk, &why));
        CHECK(set.add_cycle(run, &why));
        CHECK(set.add_cycle(sprint, &why));
        set.set_idle(idle);
        set.set_jump_start(jump_start);
        set.set_air(air);
        set.set_land(land);
    }
};

} // namespace

TEST_CASE("Clip analysis: ground speed, stride, foot phase and loop quality match the gait they were built from") {
    const fixtures::Biped b = fixtures::make_biped();
    for (const float speed : {1.0f, 1.8f, 3.5f, 6.0f}) {
        const float cycle = 1.4f / (0.6f + speed * 0.4f);
        const float stance = std::clamp(0.62f - speed * 0.05f, 0.25f, 0.6f);
        const gfx::AnimationClip clip = fixtures::make_gait(b, speed, cycle, stance, 0.1f);
        const ClipInfo info = analyze_cycle(b.skin, b.map, b.rig, clip);
        REQUIRE(info.valid);
        CHECK(std::fabs(info.ground_speed - speed) < 0.04f * speed + 0.02f);
        CHECK(std::fabs(info.stride_length - speed * cycle) < 0.04f * speed * cycle + 0.03f);
        CHECK(info.loop_error < 0.01f);
        CHECK(std::fabs(info.root_speed) < 0.02f);                        // an in-place clip
        CHECK(std::fabs(info.left.contact_start) < 0.04f || std::fabs(info.left.contact_start - 1.0f) < 0.04f);  // the left foot lands at phase 0
        CHECK(std::fabs(info.right.contact_start - 0.5f) < 0.04f);        // the right foot half a cycle later
        CHECK(std::fabs(info.left.planted_fraction - stance) < 0.06f);
    }
    // The same gait with root motion: the root speed is the travel speed, and the ground speed is unchanged (it is measured on the in-place version).
    const gfx::AnimationClip drift = fixtures::make_gait(b, 2.0f, 0.9f, 0.5f, 0.1f, true);
    const ClipInfo di = analyze_cycle(b.skin, b.map, b.rig, drift);
    REQUIRE(di.valid);
    CHECK(std::fabs(di.root_speed - 2.0f) < 0.05f);
    CHECK(std::fabs(di.ground_speed - 2.0f) < 0.1f);
    // And its in-place evaluation stays near the origin over the cycle instead of travelling.
    const gfx::ClipBinding bind = gfx::bind_clip(drift, b.skin);
    float lo = 1e9f, hi = -1e9f;
    for (int k = 0; k < 20; ++k) {
        gfx::Pose pose = gfx::rest_pose(b.skin);
        evaluate_in_place(b.skin, b.map, b.rig, drift, bind, 0.9f * static_cast<float>(k) / 20.0f, di, pose);
        const float x = joint_world(b.skin, pose)[static_cast<std::size_t>(b.map[Bone::Hips])].position.x;
        lo = std::min(lo, x);
        hi = std::max(hi, x);
    }
    CHECK(hi - lo < 0.05f);
    // Not a locomotion clip: a static pose has no planted-foot cycle.
    CHECK(!analyze_cycle(b.skin, b.map, b.rig, fixtures::make_pose_clip(b, 1.0f, 0.0f, 0.0f)).valid);
}

TEST_CASE("Locomotion set: cycles are ordered by speed, bad clips are refused with a reason") {
    World w;
    REQUIRE(w.set.cycles().size() == 3);
    CHECK(w.set.cycles()[0].info.ground_speed < w.set.cycles()[1].info.ground_speed);
    CHECK(w.set.cycles()[1].info.ground_speed < w.set.cycles()[2].info.ground_speed);
    std::string why;
    LocomotionSet s(w.biped.skin, w.biped.map);
    CHECK(!s.add_cycle(w.idle, &why));
    CHECK(!why.empty());
    CHECK(!s.usable());
    // A clip within 0.15 m/s of an existing cycle does not add a duplicate.
    CHECK(s.add_cycle(w.walk));
    CHECK(s.add_cycle(fixtures::make_gait(w.biped, 1.25f, 1.05f, 0.6f, 0.08f)));
    CHECK(s.cycles().size() == 1);
    bool threw = false;
    try { LocomotionController c(s, {}); LocomotionSet empty(w.biped.skin, w.biped.map); LocomotionController d(empty, {}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

TEST_CASE("Locomotion: feet do not slide at any speed, and the control without stride matching does") {
    World w;
    LocomotionConfig matched;
    // At the speeds of the cycles themselves (and beyond the ends of the range, where one cycle plays faster or slower) the planted feet are still: under 6 cm/s on average.
    for (const float v : {0.4f, 0.8f, 1.2f, 3.5f, 7.0f, 8.5f}) {
        const metrics::Slide s = metrics::measure_slide(w.set, matched, [&](float) { return v; }, 6.0f, 1.5f);
        CHECK(s.planted > 20);
        CHECK(s.mean < 0.06);
    }
    // Between two cycles the blended stance windows only partly overlap, so a little slides: still an order of magnitude under the speed itself.
    for (const float v : {1.7f, 2.4f, 4.6f, 6.0f}) {
        const metrics::Slide s = metrics::measure_slide(w.set, matched, [&](float) { return v; }, 6.0f);
        CHECK(s.planted > 20);
        CHECK(s.mean < 0.35);
        CHECK(s.mean < 0.12 * static_cast<double>(v) + 0.1);
    }
    // The control: playing the cycles at their authored rate regardless of the speed. Beyond the fastest cycle (8.5 m/s against 7) and below the slowest without an idle to blend with, the feet slide.
    LocomotionConfig naive;
    naive.stride_matching = false;
    const metrics::Slide fast_naive = metrics::measure_slide(w.set, naive, [](float) { return 8.5f; }, 6.0f);
    const metrics::Slide fast_matched = metrics::measure_slide(w.set, matched, [](float) { return 8.5f; }, 6.0f);
    CHECK(fast_naive.mean > 1.0);
    CHECK(fast_matched.mean < 0.06);
    LocomotionSet no_idle(w.biped.skin, w.biped.map);
    CHECK(no_idle.add_cycle(w.walk));
    CHECK(no_idle.add_cycle(w.run));
    const metrics::Slide slow_naive = metrics::measure_slide(no_idle, naive, [](float) { return 0.6f; }, 6.0f);
    const metrics::Slide slow_matched = metrics::measure_slide(no_idle, matched, [](float) { return 0.6f; }, 6.0f);
    CHECK(slow_naive.mean > 0.4);
    CHECK(slow_matched.mean < 0.06);
}

TEST_CASE("Locomotion: cycles authored with their steps at different phases still blend in step (phase alignment)") {
    // The same three gaits, but each starts at a different point of its stride (left foot landing at phase 0, 0.37 and 0.71 of the clip). Without aligning the left-foot landing, a blend of two of them
    // would put one foot down while the other lifts.
    const fixtures::Biped b = fixtures::make_biped();
    LocomotionSet shifted(b.skin, b.map);
    CHECK(shifted.add_cycle(fixtures::make_gait(b, 1.2f, 1.10f, 0.60f, 0.08f, false, 120, 0.00f)));
    CHECK(shifted.add_cycle(fixtures::make_gait(b, 3.5f, 0.70f, 0.38f, 0.20f, false, 120, 0.37f)));
    CHECK(shifted.add_cycle(fixtures::make_gait(b, 7.0f, 0.55f, 0.28f, 0.30f, false, 120, 0.71f)));
    shifted.set_idle(fixtures::make_pose_clip(b, 2.0f, 0.0f, 0.0f));
    REQUIRE(shifted.cycles().size() == 3);
    CHECK(std::fabs(shifted.cycles()[1].info.left.contact_start - shifted.cycles()[0].info.left.contact_start) > 0.2f);  // really authored out of step
    for (const float v : {2.4f, 4.6f, 6.0f}) {
        const metrics::Slide s = metrics::measure_slide(shifted, {}, [&](float) { return v; }, 6.0f, 1.5f);
        CHECK(s.planted > 40);
        CHECK(s.mean < 0.35);
    }
    const metrics::Slide at = metrics::measure_slide(shifted, {}, [](float) { return 3.5f; }, 6.0f, 1.5f);
    CHECK(at.mean < 0.06);
}

TEST_CASE("Locomotion: sliding stays small while the speed changes (acceleration, braking), and no pose pops") {
    World w;
    LocomotionConfig cfg;
    // Speed ramps 0 -> 7 -> 0 over 10 seconds.
    const auto ramp = [](float t) { return t < 5.0f ? 1.4f * t : 7.0f - 1.4f * (t - 5.0f); };
    const metrics::Slide s = metrics::measure_slide(w.set, cfg, ramp, 10.0f, 0.3f);
    CHECK(s.planted > 40);
    CHECK(s.mean < 0.35);  // the speed changes under the feet, so a little slide is expected; it must stay small
    // The largest pose change in a tick of the ramp, against what the fastest gait does by itself at its own speed.
    const metrics::Slide steady = metrics::measure_slide(w.set, cfg, [](float) { return 7.0f; }, 4.0f);
    CHECK(s.max_step < steady.max_step * 1.25 + 0.05);
    // Idle to walk to idle: no step larger than the walk's own.
    const metrics::Slide walk_ramp = metrics::measure_slide(w.set, cfg, [](float t) { return t < 2.0f ? 0.0f : (t < 6.0f ? 1.2f : 0.0f); }, 9.0f, 0.1f);
    const metrics::Slide walk_steady = metrics::measure_slide(w.set, cfg, [](float) { return 1.2f; }, 4.0f);
    CHECK(walk_ramp.max_step < walk_steady.max_step * 2.5 + 0.03);
}

TEST_CASE("Locomotion: cycle blending is a partition of unity and the left foot lands at the same shared phase in every cycle") {
    World w;
    LocomotionController c(w.set, {});
    for (const float v : {0.0f, 0.5f, 1.2f, 2.0f, 3.5f, 5.0f, 7.0f, 12.0f}) {
        for (int i = 0; i < 120; ++i) c.update(1.0f / 60.0f, {v, true, 0.0f});
        float sum = c.idle_weight();
        for (const float x : c.cycle_weights()) {
            CHECK(x >= 0.0f);
            sum += x;
        }
        CHECK(std::fabs(sum - 1.0f) < 1e-4f);
        CHECK(c.phase() >= 0.0f && c.phase() < 1.0f);
    }
    // Sampling every cycle at the shared reference phase puts its left toe on the ground at the start of its stance, in front of the body.
    for (const LocomotionCycle& cy : w.set.cycles()) {
        gfx::Pose pose = gfx::rest_pose(w.biped.skin);
        evaluate_in_place(w.biped.skin, w.biped.map, w.biped.rig, cy.clip, cy.binding, cy.info.left.contact_start * cy.clip.duration, cy.info, pose);
        const Float3 toe = metrics::contact(w.set, pose, true);
        CHECK(toe.z < 0.02f);
        CHECK(toe.x > 0.05f);
    }
}

TEST_CASE("Locomotion: states follow the character through a jump, a fall and a landing") {
    World w;
    const float dt = 1.0f / 60.0f;
    // Walk, jump (leaves the ground rising), peak, fall, land, walk on.
    {
        LocomotionController c(w.set, {});
        std::vector<LocomotionState> seen;
        const auto note = [&] {
            if (seen.empty() || seen.back() != c.state()) seen.push_back(c.state());
        };
        for (int i = 0; i < 60; ++i) { c.update(dt, {3.0f, true, 0.0f}); note(); }
        for (int i = 0; i < 12; ++i) { c.update(dt, {3.0f, false, 4.0f - 9.8f * dt * static_cast<float>(i)}); note(); }  // rising
        for (int i = 0; i < 50; ++i) { c.update(dt, {3.0f, false, -2.0f}); note(); }                                          // falling
        for (int i = 0; i < 90; ++i) { c.update(dt, {3.0f, true, 0.0f}); note(); }                                            // landed, moving
        const std::vector<LocomotionState> want = {LocomotionState::Ground, LocomotionState::JumpStart, LocomotionState::Air, LocomotionState::Land, LocomotionState::Ground};
        CHECK(seen == want);
    }
    // Walking off a ledge: no takeoff, straight to the air loop.
    {
        LocomotionController c(w.set, {});
        for (int i = 0; i < 30; ++i) c.update(dt, {2.0f, true, 0.0f});
        for (int i = 0; i < 10; ++i) c.update(dt, {2.0f, false, -1.0f - static_cast<float>(i)});
        CHECK(c.state() == LocomotionState::Air);
    }
    // A hop that lands before the takeoff finishes goes to the landing, not back to the ground blend or the air.
    {
        LocomotionController c(w.set, {});
        for (int i = 0; i < 30; ++i) c.update(dt, {0.0f, true, 0.0f});
        for (int i = 0; i < 6; ++i) c.update(dt, {0.0f, false, 2.5f});
        CHECK(c.state() == LocomotionState::JumpStart);
        for (int i = 0; i < 3; ++i) c.update(dt, {0.0f, true, 0.0f});
        CHECK(c.state() == LocomotionState::Land);
    }
    // Standing still lands, then stands.
    {
        LocomotionController c(w.set, {});
        for (int i = 0; i < 20; ++i) c.update(dt, {0.0f, true, 0.0f});
        for (int i = 0; i < 20; ++i) c.update(dt, {0.0f, false, 3.0f - 0.3f * static_cast<float>(i)});
        for (int i = 0; i < 80; ++i) c.update(dt, {0.0f, true, 0.0f});
        CHECK(c.state() == LocomotionState::Ground);
        CHECK(c.idle_weight() > 0.99f);
    }
}

TEST_CASE("Locomotion: transitions between states are continuous (no pose pops through a jump)") {
    World w;
    const float dt = 1.0f / 60.0f;
    LocomotionController c(w.set, {});
    c.update(dt, {4.0f, true, 0.0f});  // the first tick leaves the rest pose: not a transition
    gfx::Pose last = c.pose();
    float worst = 0.0f, worst_t = 0.0f;
    const auto step = [&](float speed, bool grounded, float vy) {
        c.update(dt, {speed, grounded, vy});
        float tr = 0.0f;
        worst = std::max(worst, pose_distance(c.pose(), last, &tr));
        worst_t = std::max(worst_t, tr);
        last = c.pose();
    };
    for (int i = 0; i < 90; ++i) step(4.0f, true, 0.0f);
    for (int i = 0; i < 30; ++i) step(4.0f, false, 4.0f - 0.3f * static_cast<float>(i));
    for (int i = 0; i < 40; ++i) step(4.0f, false, -4.0f);
    for (int i = 0; i < 90; ++i) step(4.0f, true, 0.0f);
    // The animation itself moves by up to this much per tick (the sprint's), so a transition must stay within a small multiple.
    const metrics::Slide steady = metrics::measure_slide(w.set, {}, [](float) { return 7.0f; }, 3.0f);
    CHECK(static_cast<double>(worst) < steady.max_step * 1.6 + 0.1);
    CHECK(worst_t < 0.15f);  // meters of joint translation per tick
}

TEST_CASE("Locomotion: bad input is rejected, a stationary tick does not advance the cycle, and a tick is deterministic") {
    World w;
    LocomotionController c(w.set, {});
    bool threw = false;
    try { c.update(0.0f, {1.0f, true, 0.0f}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { c.update(1.0f / 60.0f, {std::nanf(""), true, 0.0f}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { c.update(std::numeric_limits<float>::infinity(), {1.0f, true, 0.0f}); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    // A negative speed is clamped to rest.
    for (int i = 0; i < 60; ++i) c.update(1.0f / 60.0f, {-3.0f, true, 0.0f});
    CHECK(c.idle_weight() > 0.99f);
    const float phase = c.phase();
    for (int i = 0; i < 60; ++i) c.update(1.0f / 60.0f, {0.0f, true, 0.0f});
    CHECK(std::fabs(c.phase() - phase) < 1e-4f);
    // Two controllers fed the same inputs produce bit-identical poses.
    LocomotionController a(w.set, {}), b(w.set, {});
    for (int i = 0; i < 300; ++i) {
        const float v = 3.0f + 3.0f * std::sin(static_cast<float>(i) * 0.05f);
        a.update(1.0f / 60.0f, {v, true, 0.0f});
        b.update(1.0f / 60.0f, {v, true, 0.0f});
    }
    CHECK(pose_distance(a.pose(), b.pose()) == 0.0f);
}
