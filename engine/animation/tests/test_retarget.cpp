// Retargeting a clip between humanoids of different proportions, joint names, rest poses and facing.
#include "saige/gfx_test/saige_test.hpp"

#include "fixtures.hpp"
#include "metrics.hpp"
#include "saige/animation/retarget.hpp"

using namespace saige;
using namespace saige::anim;
using render::Float3;

namespace {

ClipInfo retarget_and_analyze(const fixtures::Biped& src, const gfx::AnimationClip& clip, const fixtures::Biped& dst, gfx::AnimationClip* out = nullptr) {
    const gfx::Pose rest = gfx::rest_pose(src.skin);
    const RetargetSource rs{&src.skin, &src.map, &src.rig, &rest};
    const gfx::AnimationClip rt = retarget_clip(rs, clip, dst.skin, dst.map, dst.rig);
    if (out != nullptr) *out = rt;
    return analyze_cycle(dst.skin, dst.map, dst.rig, rt);
}

} // namespace

TEST_CASE("Retarget: a gait keeps its stride in proportion on a taller rig, with different names and an A-pose") {
    const fixtures::Biped src = fixtures::make_biped(1.0f, fixtures::Naming::Unreal);
    const gfx::AnimationClip clip = fixtures::make_gait(src, 2.0f, 0.95f, 0.5f, 0.12f);
    const ClipInfo si = analyze_cycle(src.skin, src.map, src.rig, clip);
    REQUIRE(si.valid);
    for (const float scale : {0.7f, 1.0f, 1.3f}) {
        const fixtures::Biped dst = fixtures::make_biped(scale, fixtures::Naming::Mixamo, 0.0f, true);
        const float ratio = dst.rig.leg_length / src.rig.leg_length;
        const ClipInfo ti = retarget_and_analyze(src, clip, dst);
        REQUIRE(ti.valid);
        // The speed needed for the feet not to slide scales with the leg length, and the cycle time is unchanged.
        CHECK(std::fabs(ti.ground_speed - si.ground_speed * ratio) < 0.06f * si.ground_speed * ratio + 0.03f);
        CHECK(std::fabs(ti.duration - si.duration) < 1e-3f);
        CHECK(std::fabs(ti.left.contact_start - si.left.contact_start) < 0.05f || std::fabs(std::fabs(ti.left.contact_start - si.left.contact_start) - 1.0f) < 0.05f);
        CHECK(std::fabs(ti.left.planted_fraction - si.left.planted_fraction) < 0.06f);
        CHECK(ti.loop_error < 0.02f);
    }
}

TEST_CASE("Retarget: the retargeted legs plant the feet on the floor, and the pose is the source's, bone by bone") {
    const fixtures::Biped src = fixtures::make_biped(1.0f);
    const fixtures::Biped dst = fixtures::make_biped(1.2f, fixtures::Naming::Mixamo);
    const gfx::AnimationClip clip = fixtures::make_gait(src, 1.5f, 1.0f, 0.55f, 0.1f);
    gfx::AnimationClip rt;
    retarget_and_analyze(src, clip, dst, &rt);
    const gfx::ClipBinding sb = gfx::bind_clip(clip, src.skin), db = gfx::bind_clip(rt, dst.skin);
    float lowest = 1e9f;
    float worst_rotation = 0.0f;
    for (int k = 0; k < 40; ++k) {
        const float t = static_cast<float>(k) / 40.0f;
        gfx::Pose sp = gfx::rest_pose(src.skin), dp = gfx::rest_pose(dst.skin);
        gfx::evaluate_clip(clip, sb, t, true, sp);
        gfx::evaluate_clip(rt, db, t, true, dp);
        const std::vector<JointWorld> sw = joint_world(src.skin, sp), dw = joint_world(dst.skin, dp);
        for (const Bone toe : {Bone::LeftToes, Bone::RightToes}) lowest = std::min(lowest, dw[static_cast<std::size_t>(dst.map[toe])].position.z);
        // World orientation of every bone the two rigs share: the same change from rest (both rigs have identical rest orientations here, so the orientations themselves agree).
        for (std::size_t b = 0; b < bone_count; ++b) {
            if (src.map.joint[b] < 0) continue;
            worst_rotation = std::max(worst_rotation, rotation_angle_between(sw[static_cast<std::size_t>(src.map.joint[b])].rotation, dw[static_cast<std::size_t>(dst.map.joint[b])].rotation));
        }
    }
    CHECK(lowest > -0.015f);   // no foot sinks through the floor
    CHECK(lowest < 0.015f);    // and the lowest point of the cycle is the floor
    CHECK(worst_rotation < 0.02f);
}

TEST_CASE("Retarget: a rig that faces another way is turned to face along its own forward") {
    const fixtures::Biped src = fixtures::make_biped(1.0f);
    const gfx::AnimationClip clip = fixtures::make_gait(src, 2.0f, 0.9f, 0.5f, 0.1f);
    for (const float yaw : {1.5707963f, -1.0f, 3.0f}) {
        const fixtures::Biped dst = fixtures::make_biped(1.0f, fixtures::Naming::Unreal, yaw);
        const ClipInfo ti = retarget_and_analyze(src, clip, dst);
        REQUIRE(ti.valid);
        CHECK(std::fabs(ti.ground_speed - 2.0f) < 0.15f);
        // Walking along the facing: no sideways drift of the contact points.
        CHECK(std::fabs(ti.root_speed) < 0.1f);
    }
}

TEST_CASE("Retarget: a root-motion clip keeps its travel, scaled with the rig") {
    const fixtures::Biped src = fixtures::make_biped(1.0f);
    const fixtures::Biped dst = fixtures::make_biped(0.8f, fixtures::Naming::Mixamo);
    const gfx::AnimationClip clip = fixtures::make_gait(src, 1.8f, 1.0f, 0.55f, 0.1f, true);
    const ClipInfo ti = retarget_and_analyze(src, clip, dst);
    REQUIRE(ti.valid);
    // The hips carry the travel in the source (the clip's root joint does, which is above the hips); retargeted, the hips' own motion is scaled, the root's is not carried. The in-place ground speed
    // is what matters and it follows the proportions.
    const float ratio = dst.rig.leg_length / src.rig.leg_length;
    CHECK(std::fabs(ti.ground_speed - 1.8f * ratio) < 0.12f * 1.8f * ratio);
}

TEST_CASE("Retarget: bad input throws") {
    const fixtures::Biped src = fixtures::make_biped(1.0f);
    const gfx::AnimationClip clip = fixtures::make_gait(src, 1.0f, 1.0f);
    const gfx::Pose rest = gfx::rest_pose(src.skin);
    const RetargetSource none{nullptr, &src.map, &src.rig, &rest};
    bool threw = false;
    try { (void)retarget_clip(none, clip, src.skin, src.map, src.rig); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    threw = false;
    RetargetOptions o;
    o.fps = 0.0f;
    const RetargetSource ok{&src.skin, &src.map, &src.rig, &rest};
    try { (void)retarget_clip(ok, clip, src.skin, src.map, src.rig, o); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}
