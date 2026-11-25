// Upper-body layering and look-at: the part of the body a layer does not drive is the locomotion's, bit for bit; the head ends up facing the target within the limits, and no further.
#include "saige/gfx_test/saige_test.hpp"

#include "fixtures.hpp"
#include "saige/animation/layers.hpp"

#include <cmath>
#include <stdexcept>

using namespace saige;
using namespace saige::anim;
using render::Float3;
using render::Quat;

namespace {

template <class F>
bool throws(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

constexpr float deg = 3.14159265f / 180.0f;

// Where the head faces in model space.
Float3 head_facing(const fixtures::Biped& b, const gfx::Pose& pose) {
    const std::size_t head = static_cast<std::size_t>(b.map[Bone::Head]);
    const Quat rest = joint_world(b.skin, gfx::rest_pose(b.skin))[head].rotation;
    return render::rotate(joint_world(b.skin, pose)[head].rotation, render::rotate(render::conjugate(rest), b.rig.forward));
}

float yaw_deg(const fixtures::Biped& b, Float3 d) { return std::atan2(render::dot(d, b.rig.left), render::dot(d, b.rig.forward)) / deg; }
float pitch_deg(Float3 d) { return std::atan2(d.z, std::hypot(d.x, d.y)) / deg; }

Float3 head_position(const fixtures::Biped& b, const gfx::Pose& pose) { return joint_world(b.skin, pose)[static_cast<std::size_t>(b.map[Bone::Head])].position; }

const render::Transform identity_root{{0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1}};

bool same_rotation(const gfx::Pose& a, const gfx::Pose& b, std::size_t j) { return a.local[j].rotation == b.local[j].rotation && a.local[j].position == b.local[j].position; }

} // namespace

TEST_CASE("Layers: the upper-body mask covers the spine and above and nothing below") {
    const fixtures::Biped b = fixtures::make_biped();
    const BoneMask mask = upper_body_mask(b.skin, b.map, 0.5f);
    REQUIRE(mask.weight.size() == b.skin.joint_count());
    const auto w = [&](Bone bone) { return mask.weight[static_cast<std::size_t>(b.map[bone])]; };
    CHECK(w(Bone::Hips) == 0.0f);
    CHECK(w(Bone::LeftUpperLeg) == 0.0f);
    CHECK(w(Bone::RightFoot) == 0.0f);
    CHECK(w(Bone::LeftToes) == 0.0f);
    CHECK(w(Bone::Spine) == 0.5f);
    CHECK(w(Bone::Chest) == 1.0f);
    CHECK(w(Bone::Head) == 1.0f);
    CHECK(w(Bone::LeftHand) == 1.0f);
    CHECK(w(Bone::RightUpperArm) == 1.0f);
    // A mask below the left leg only.
    const BoneMask leg = mask_below(b.skin, {b.map[Bone::LeftUpperLeg], -1});
    CHECK(leg.weight[static_cast<std::size_t>(b.map[Bone::LeftToes])] == 1.0f);
    CHECK(leg.weight[static_cast<std::size_t>(b.map[Bone::RightUpperLeg])] == 0.0f);
    CHECK(leg.weight[static_cast<std::size_t>(b.map[Bone::Spine])] == 0.0f);
}

TEST_CASE("Layers: a masked blend leaves the legs exactly the locomotion's and gives the arms the layer's pose") {
    const fixtures::Biped b = fixtures::make_biped();
    LocomotionSet set(b.skin, b.map);
    REQUIRE(set.add_cycle(fixtures::make_gait(b, 3.5f, 0.70f, 0.38f, 0.20f)));
    set.set_idle(fixtures::make_pose_clip(b, 2.0f, 0.0f, 0.0f));
    LocomotionController run(set, {});
    for (int i = 0; i < 90; ++i) run.update(1.0f / 60.0f, {3.5f, true, 0.0f});
    const gfx::Pose base = run.pose();
    // A layer that raises both arms.
    const gfx::AnimationClip lift = fixtures::make_pose_clip(b, 1.0f, 0.0f, 1.2f);
    gfx::Pose layer = gfx::rest_pose(b.skin);
    gfx::evaluate_clip(lift, gfx::bind_clip(lift, b.skin), 1.0f, false, layer);
    const BoneMask mask = upper_body_mask(b.skin, b.map);
    gfx::Pose full = base, half = base, none = base;
    blend_masked(full, layer, mask, 1.0f);
    blend_masked(half, layer, mask, 0.5f);
    blend_masked(none, layer, mask, 0.0f);
    for (const Bone leg : {Bone::Hips, Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot, Bone::LeftToes, Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot, Bone::RightToes}) {
        const std::size_t j = static_cast<std::size_t>(b.map[leg]);
        CHECK(same_rotation(full, base, j));
        CHECK(same_rotation(half, base, j));
    }
    CHECK(pose_distance(none, base) == 0.0f);
    for (const Bone arm : {Bone::LeftUpperArm, Bone::RightUpperArm}) {
        const std::size_t j = static_cast<std::size_t>(b.map[arm]);
        CHECK(render::length(Float3{full.local[j].rotation.x - layer.local[j].rotation.x, full.local[j].rotation.y - layer.local[j].rotation.y, full.local[j].rotation.z - layer.local[j].rotation.z}) < 1e-5f);
        const float full_angle = rotation_angle_between(base.local[j].rotation, full.local[j].rotation), half_angle = rotation_angle_between(base.local[j].rotation, half.local[j].rotation);
        CHECK(full_angle > 1.0f);
        CHECK(std::fabs(half_angle - 0.5f * full_angle) < 0.02f);
    }
    // Bad sizes throw.
    gfx::Pose small;
    CHECK(throws([&] { blend_masked(small, layer, mask, 1.0f); }));
    BoneMask wrong;
    wrong.weight.assign(3, 1.0f);
    CHECK(throws([&] { blend_masked(full, layer, wrong, 1.0f); }));
}

TEST_CASE("Look at: the head faces a target within the limits, from any heading, and only rotations change") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::Pose rest = gfx::rest_pose(b.skin);
    const Float3 eye = head_position(b, rest);
    struct Case {
        float yaw, pitch;
    };
    for (const render::Transform& root : {identity_root, render::Transform{{3, -2, 0}, render::from_axis_angle({0, 0, 1}, 1.2f), {1, 1, 1}}}) {
        for (const Case c : {Case{0, 0}, Case{40, 0}, Case{-55, 10}, Case{25, 30}, Case{-10, -25}}) {
            gfx::Pose pose = rest;
            LookAtState state;
            const Float3 dir = b.rig.forward * (std::cos(c.pitch * deg) * std::cos(c.yaw * deg)) + b.rig.left * (std::cos(c.pitch * deg) * std::sin(c.yaw * deg)) + Float3{0, 0, std::sin(c.pitch * deg)};
            const Float3 target = root.position + render::rotate(root.rotation, eye + dir * 6.0f);
            const float remaining = look_at(b.skin, b.map, b.rig, pose, root, target, {}, state, 1.0f, 1.0f / 60.0f);
            const Float3 face = head_facing(b, pose);
            CHECK(std::fabs(yaw_deg(b, face) - c.yaw) < 1.5f);
            CHECK(std::fabs(pitch_deg(face) - c.pitch) < 1.5f);
            CHECK(remaining < 2.0f * deg);
            // Rotations only: no joint moved, none got longer.
            for (std::size_t j = 0; j < pose.local.size(); ++j) CHECK(pose.local[j].position == rest.local[j].position);
            // The legs and the hips are untouched.
            for (const Bone leg : {Bone::Hips, Bone::LeftUpperLeg, Bone::RightFoot, Bone::LeftToes}) CHECK(same_rotation(pose, rest, static_cast<std::size_t>(b.map[leg])));
        }
    }
}

TEST_CASE("Look at: the turn is shared among the spine, chest, neck and head by their shares, not taken by the head alone") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::Pose rest = gfx::rest_pose(b.skin);
    const Float3 eye = head_position(b, rest);
    const LookAtConfig cfg;
    gfx::Pose pose = rest;
    LookAtState state;
    (void)look_at(b.skin, b.map, b.rig, pose, identity_root, eye + Float3{5.0f * std::cos(40.0f * deg), 5.0f * std::sin(40.0f * deg), 0.0f}, cfg, state, 1.0f, 1.0f / 60.0f);
    const auto turned = [&](Bone bone) {
        const std::size_t j = static_cast<std::size_t>(b.map[bone]);
        return rotation_angle_between(rest.local[j].rotation, pose.local[j].rotation) / deg;
    };
    CHECK(std::fabs(turned(Bone::Spine) - 0.15f * 40.0f) < 2.0f);
    CHECK(std::fabs(turned(Bone::Chest) - 0.25f * 40.0f) < 2.0f);
    CHECK(std::fabs(turned(Bone::Neck) - 0.25f * 40.0f) < 2.0f);
    CHECK(std::fabs(turned(Bone::Head) - 0.35f * 40.0f) < 2.0f);
    // The shoulders and arms are carried by the chest, not turned on their own.
    CHECK(turned(Bone::LeftUpperArm) < 0.5f);
}

TEST_CASE("Look at: the limits hold for a target behind or far above, the weight scales the turn, and a bad input throws") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::Pose rest = gfx::rest_pose(b.skin);
    const Float3 eye = head_position(b, rest);
    const LookAtConfig cfg;
    LookAtState behind;
    gfx::Pose pose = rest;
    (void)look_at(b.skin, b.map, b.rig, pose, identity_root, eye + Float3{-5, 1.0f, 0.0f}, cfg, behind, 1.0f, 1.0f / 60.0f);
    Float3 face = head_facing(b, pose);
    CHECK(std::fabs(std::fabs(yaw_deg(b, face)) - cfg.max_yaw / deg) < 1.5f);   // at the limit, not past it
    CHECK(yaw_deg(b, face) > 0.0f);                                              // over the nearer (left) shoulder
    LookAtState up;
    pose = rest;
    const float remaining = look_at(b.skin, b.map, b.rig, pose, identity_root, eye + Float3{0.2f, 0.0f, 8.0f}, cfg, up, 1.0f, 1.0f / 60.0f);
    face = head_facing(b, pose);
    CHECK(std::fabs(pitch_deg(face) - cfg.max_pitch_up / deg) < 1.5f);
    CHECK(remaining > 30.0f * deg);                                              // it reports that it could not reach the target
    // Weight 0 leaves the pose bit-identical; weight 0.5 turns half as far.
    LookAtState s0, s1, s2;
    gfx::Pose p0 = rest, p1 = rest, p2 = rest;
    const Float3 side = eye + Float3{3.0f, 3.0f, 0.0f};  // 45 degrees to the left
    (void)look_at(b.skin, b.map, b.rig, p0, identity_root, side, cfg, s0, 0.0f, 1.0f / 60.0f);
    (void)look_at(b.skin, b.map, b.rig, p1, identity_root, side, cfg, s1, 1.0f, 1.0f / 60.0f);
    (void)look_at(b.skin, b.map, b.rig, p2, identity_root, side, cfg, s2, 0.5f, 1.0f / 60.0f);
    CHECK(pose_distance(p0, rest) == 0.0f);
    CHECK(std::fabs(yaw_deg(b, head_facing(b, p1)) - 45.0f) < 1.5f);
    CHECK(std::fabs(yaw_deg(b, head_facing(b, p2)) - 22.5f) < 1.5f);
    // Bad input.
    LookAtState s;
    CHECK(throws([&] { (void)look_at(b.skin, b.map, b.rig, pose, identity_root, {std::nanf(""), 0, 0}, cfg, s, 1.0f, 1.0f / 60.0f); }));
    CHECK(throws([&] { (void)look_at(b.skin, b.map, b.rig, pose, identity_root, side, cfg, s, 1.0f, 0.0f); }));
}

TEST_CASE("Look at: a target that jumps is followed smoothly, without overshoot, in about the smoothing time") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::Pose rest = gfx::rest_pose(b.skin);
    const Float3 eye = head_position(b, rest);
    LookAtConfig cfg;
    cfg.smoothing = 0.2f;
    LookAtState state;
    gfx::Pose pose = rest;
    (void)look_at(b.skin, b.map, b.rig, pose, identity_root, eye + Float3{5, 0, 0}, cfg, state, 1.0f, 1.0f / 60.0f);  // looking ahead
    CHECK(std::fabs(yaw_deg(b, head_facing(b, pose))) < 1.0f);
    const Float3 jump = eye + Float3{0, 5, 0};  // 90 degrees to the left: beyond the 70 degree limit
    float previous = 0.0f, at_tau = 0.0f, last = 0.0f;
    bool monotone = true, overshoot = false;
    for (int i = 1; i <= 120; ++i) {
        pose = rest;
        (void)look_at(b.skin, b.map, b.rig, pose, identity_root, jump, cfg, state, 1.0f, 1.0f / 60.0f);
        const float yaw = yaw_deg(b, head_facing(b, pose));
        monotone = monotone && yaw >= previous - 0.05f;
        overshoot = overshoot || yaw > cfg.max_yaw / deg + 1.5f;
        previous = yaw;
        if (i == 12) at_tau = yaw;  // 0.2 s
        last = yaw;
    }
    CHECK(monotone);
    CHECK(!overshoot);
    CHECK(at_tau > 0.55f * cfg.max_yaw / deg && at_tau < 0.72f * cfg.max_yaw / deg);  // 1 - 1/e = 63% of the way after one time constant
    CHECK(std::fabs(last - cfg.max_yaw / deg) < 1.5f);
}

TEST_CASE("Stance blend: the weight ramps over the fade time in either direction and never jumps; the crouch capsule height follows the pose") {
    StanceBlend blend(0.2f);
    CHECK(blend.weight() == 0.0f);
    CHECK(blend.settled());
    float previous = 0.0f, biggest_step = 0.0f;
    int ticks = 0;
    while (!(blend.weight() >= 1.0f) && ticks < 100) {
        blend.update(1.0f / 60.0f, true);
        biggest_step = std::max(biggest_step, blend.weight() - previous);
        previous = blend.weight();
        ++ticks;
    }
    CHECK(ticks >= 12 && ticks <= 13);                     // 0.2 s at 60 Hz
    CHECK(biggest_step < 1.0f / 12.0f + 1e-4f);
    CHECK(blend.settled());
    for (int i = 0; i < 6; ++i) blend.update(1.0f / 60.0f, false);  // back up for 0.1 s: halfway
    CHECK(std::fabs(blend.weight() - 0.5f) < 0.02f);
    CHECK(!blend.settled());
    for (int i = 0; i < 30; ++i) blend.update(1.0f / 60.0f, false);
    CHECK(blend.weight() == 0.0f);
    CHECK(throws([&] { blend.update(0.0f, true); }));
    // The crouch height: head 0.94 m crouched, standing head 1.52 m under a 1.7 m capsule -> 0.94 + 0.18.
    CHECK(std::fabs(crouch_capsule_height(0.94f, 1.52f, 1.7f, 0.6f) - 1.12f) < 1e-5f);
    CHECK(crouch_capsule_height(0.2f, 1.52f, 1.7f, 0.6f) == 0.6f);          // never under the minimum
    CHECK(crouch_capsule_height(1.6f, 1.52f, 1.7f, 0.6f) == 1.7f);          // never above standing
}

TEST_CASE("Additive layer: the layer's change from its reference is added on top of what the body does, scaled by weight and mask, and nothing changes when the layer is its reference") {
    const fixtures::Biped b = fixtures::make_biped();
    LocomotionSet set(b.skin, b.map);
    REQUIRE(set.add_cycle(fixtures::make_gait(b, 3.5f, 0.70f, 0.38f, 0.20f)));
    set.set_idle(fixtures::make_pose_clip(b, 2.0f, 0.0f, 0.0f));
    LocomotionController run(set, {});
    for (int i = 0; i < 90; ++i) run.update(1.0f / 60.0f, {3.5f, true, 0.0f});
    gfx::Pose base = run.pose();
    const gfx::Pose rest = gfx::rest_pose(b.skin);
    // Give the arms their own motion (the fixture's gait has no arm swing): swung 0.5 rad about the vertical.
    for (const Bone arm : {Bone::LeftUpperArm, Bone::RightUpperArm}) {
        const std::size_t j = static_cast<std::size_t>(b.map[arm]);
        base.local[j].rotation = render::normalize(render::from_axis_angle({0, 0, 1}, 0.5f) * base.local[j].rotation);
    }
    const gfx::AnimationClip lift = fixtures::make_pose_clip(b, 1.0f, 0.1f, 0.8f);
    gfx::Pose layer = rest;
    gfx::evaluate_clip(lift, gfx::bind_clip(lift, b.skin), 1.0f, false, layer);
    const BoneMask mask = upper_body_mask(b.skin, b.map);
    // Layer equal to its reference: nothing changes, bit for bit.
    gfx::Pose same = base;
    blend_additive(same, rest, rest, mask, 1.0f);
    CHECK(pose_distance(same, base) == 0.0f);
    // The arms turn by the layer's own change from the rest pose, on top of the run's arm swing.
    gfx::Pose full = base, half = base;
    blend_additive(full, layer, rest, mask, 1.0f);
    blend_additive(half, layer, rest, mask, 0.5f);
    for (const Bone arm : {Bone::LeftUpperArm, Bone::RightUpperArm}) {
        const std::size_t j = static_cast<std::size_t>(b.map[arm]);
        const float wanted = rotation_angle_between(rest.local[j].rotation, layer.local[j].rotation);
        const float got = rotation_angle_between(base.local[j].rotation, full.local[j].rotation);
        const float got_half = rotation_angle_between(base.local[j].rotation, half.local[j].rotation);
        CHECK(std::fabs(got - wanted) < 2e-3f);
        CHECK(std::fabs(got_half - 0.5f * wanted) < 5e-3f);
        CHECK(wanted > 0.5f);
        // Exactly the base pose with the layer's local delta applied after it: base * conj(rest) * layer.
        const Quat expected = render::normalize(base.local[j].rotation * render::conjugate(rest.local[j].rotation) * layer.local[j].rotation);
        CHECK(rotation_angle_between(full.local[j].rotation, expected) < 1e-4f);
        // Unlike a replacing layer, the run's own arm motion is kept: the result is not the layer's pose.
        CHECK(rotation_angle_between(full.local[j].rotation, layer.local[j].rotation) > 0.05f);
    }
    // The pelvis drop of the layer is masked out (the hips have mask 0), and so are the legs.
    for (const Bone leg : {Bone::Hips, Bone::LeftUpperLeg, Bone::RightFoot}) {
        const std::size_t j = static_cast<std::size_t>(b.map[leg]);
        CHECK(full.local[j].rotation == base.local[j].rotation);
        CHECK(full.local[j].position == base.local[j].position);
    }
    // Weight 0 is bit-identical; sizes are checked.
    gfx::Pose none = base;
    blend_additive(none, layer, rest, mask, 0.0f);
    CHECK(pose_distance(none, base) == 0.0f);
    gfx::Pose small;
    CHECK(throws([&] { blend_additive(small, layer, rest, mask, 1.0f); }));
}
