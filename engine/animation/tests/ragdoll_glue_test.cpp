// The skeleton to and from the humanoid ragdoll on a synthetic rig: the bodies built from a pose agree with the ragdoll's own rest pose, a round trip at the start changes nothing, and after a collapse the
// skeleton follows the bodies (the ankles stay on the shins' ends, bone lengths are exactly the rig's, the hips are at the pelvis). Exit 77 (skipped) when no dynamics backend is compiled in.
#include "saige/animation/ragdoll_glue.hpp"

#include "fixtures.hpp"

#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

using saige::BoneTransform;
using saige::DVec;
using saige::DQuat;
using saige::DynamicsBackend;
using saige::DynamicsConfig;
using saige::HumanoidSpec;
using saige::Ragdoll;
using saige::RagdollPose;
using saige::StaticCollision;
using saige::Aabb;
using saige::available_dynamics_backends;
using saige::humanoid_body_pose;
using saige::kBoneCount;
using saige::make_dynamics_world;
namespace gfx = saige::gfx;
namespace render = saige::render;
namespace anim = saige::anim;
using anim::RagdollFrame;
using anim::joint_world;
using anim::pose_distance;
using anim::pose_from_ragdoll;
using anim::spec_from_pose;

namespace {

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

double dist(DVec a, DVec b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
DVec rotate(DQuat q, DVec v) {
    const DVec u{q.x, q.y, q.z};
    const DVec t{2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z), 2 * (u.x * v.y - u.y * v.x)};
    const DVec c{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
    return {v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z};
}

} // namespace

int main() {
    if (available_dynamics_backends().empty()) {
        std::cout << "no dynamics backend is compiled in (configure with -DSAIGE_PHYSICS_JOLT=ON): skipped\n";
        return 77;
    }
    try {
        const fixtures::Biped b = fixtures::make_biped();
        const gfx::Pose rest = gfx::rest_pose(b.skin);
        const auto rest_joints_for_turn = joint_world(b.skin, rest);
        const RagdollFrame frame{{3.0, -2.0, 0.0}, 0.7};
        DynamicsConfig cfg;
        cfg.collision_steps = 4;
        cfg.velocity_iterations = 20;
        cfg.position_iterations = 10;
        cfg.allow_sleeping = false;
        auto world = make_dynamics_world(DynamicsBackend::Jolt, cfg);
        require(world != nullptr, "a backend");
        world->set_static_geometry(StaticCollision(std::vector<Aabb>{{{-100, -100, -2}, {100, 100, 0}}}, {}), nullptr);

        // 1. Bodies from a pose are the ragdoll's rest pose.
        const HumanoidSpec spec = spec_from_pose(b.skin, b.map, rest, 75.0, frame);
        std::printf("  the synthetic rig measures %.2f m tall (spec), feet at z = %.3f\n", spec.height, spec.ankle[0].z);
        Ragdoll ragdoll(*world, spec, 1);
        const RagdollPose reference = humanoid_body_pose(spec);
        double worst = 0.0;
        for (std::size_t i = 0; i < kBoneCount; ++i) worst = std::max(worst, dist(reference[i].position, ragdoll.rest_pose()[i].position));
        require(worst < 1e-9, "the bodies of the pose are the ragdoll's rest pose");

        // 2. Starting and reading straight back changes nothing.
        ragdoll.start(reference, reference, 1.0 / 60.0);
        gfx::Pose back = rest;
        pose_from_ragdoll(b.skin, b.map, back, ragdoll.pose(), reference, frame);
        float max_translation = 0.0f;
        const float rotation = pose_distance(rest, back, &max_translation);
        std::printf("  start and read straight back: largest rotation difference %.5f rad, translation %.5f m\n", static_cast<double>(rotation), static_cast<double>(max_translation));
        require(rotation < 1e-3f && max_translation < 1e-3f, "a round trip at the start changes the pose by under a milliradian and a millimeter");

        // 3. Let it fall (from a hand's breadth up) and settle; the skeleton follows.
        {
            RagdollPose lifted = reference;
            for (auto& t : lifted) t.position.z += 0.15;
            ragdoll.start(lifted, lifted, 1.0 / 60.0);
        }
        for (int i = 0; i < 60 * 5; ++i) world->step(1.0 / 60.0);
        // The reference moved with the start: the bodies' turn is measured from `lifted`, which has the rotations of `reference`.
        gfx::Pose follow = rest;
        const RagdollPose physical = ragdoll.pose();
        pose_from_ragdoll(b.skin, b.map, follow, physical, reference, frame);
        const auto world_joints = joint_world(b.skin, follow);
        const auto at = [&](anim::Bone j) { return anim::detail::to_world(frame, world_joints[static_cast<std::size_t>(b.map[j])].position); };
        // The ankles against the shins' ends, the wrists against the forearms' ends.
        const auto end_of = [&](::saige::Bone body, DVec from, DVec to, bool far_end) {
            const double len = dist(from, to);
            const BoneTransform& t = physical[static_cast<std::size_t>(body)];
            return rotate(t.rotation, DVec{0, 0, far_end ? 0.5 * len : -0.5 * len});
        };
        double miss = 0.0;
        for (int i = 0; i < 2; ++i) {
            const bool left = i == 0;
            const BoneTransform& shin = physical[static_cast<std::size_t>(left ? ::saige::Bone::ShinL : ::saige::Bone::ShinR)];
            const DVec e = end_of(left ? ::saige::Bone::ShinL : ::saige::Bone::ShinR, spec.knee[i], spec.ankle[i], true);
            const DVec expected{shin.position.x + e.x, shin.position.y + e.y, shin.position.z + e.z};
            miss = std::max(miss, dist(expected, at(left ? anim::Bone::LeftFoot : anim::Bone::RightFoot)));
            const BoneTransform& fore = physical[static_cast<std::size_t>(left ? ::saige::Bone::ForearmL : ::saige::Bone::ForearmR)];
            const DVec w = end_of(left ? ::saige::Bone::ForearmL : ::saige::Bone::ForearmR, spec.elbow[i], spec.wrist[i], true);
            const DVec expected_wrist{fore.position.x + w.x, fore.position.y + w.y, fore.position.z + w.z};
            miss = std::max(miss, dist(expected_wrist, at(left ? anim::Bone::LeftHand : anim::Bone::RightHand)));
        }
        std::printf("  after a 5 s collapse: the skeleton's ankles and wrists are within %.3f m of the bodies' ends (the pelvis is at z = %.3f)\n", miss, physical[0].position.z);
        require(miss < 0.06, "the skeleton's ankles and wrists stay within 6 cm of the ends of the shins and forearms");
        require(physical[0].position.z < 0.25, "control: the ragdoll collapsed");
        const DVec hips = at(anim::Bone::Hips);
        require(dist(hips, physical[0].position) < 1e-3, "the hips are at the pelvis");
        // Joints with no offset children follow their body's turn too (the ankles and wrists above only test the joints above them): feet, hands and the head, and the neck half way between torso and head.
        {
            const render::Quat yaw = anim::detail::yaw_quat(frame.yaw);
            const auto skeleton_turn = [&](anim::Bone j) {
                const std::size_t k = static_cast<std::size_t>(b.map[j]);
                return yaw * (joint_world(b.skin, follow)[k].rotation * render::conjugate(rest_joints_for_turn[k].rotation)) * render::conjugate(yaw);
            };
            const auto body_turn = [&](::saige::Bone body) {
                const std::size_t i = static_cast<std::size_t>(body);
                return render::normalize(anim::detail::to_quat(physical[i].rotation)) * render::conjugate(render::normalize(anim::detail::to_quat(reference[i].rotation)));
            };
            const auto angle = [](render::Quat a, render::Quat c) {
                const float d = std::fabs(a.x * c.x + a.y * c.y + a.z * c.z + a.w * c.w);
                return 2.0 * static_cast<double>(std::acos(std::min(1.0f, d)));
            };
            const double foot = angle(skeleton_turn(anim::Bone::LeftFoot), body_turn(::saige::Bone::FootL)), rfoot = angle(skeleton_turn(anim::Bone::RightFoot), body_turn(::saige::Bone::FootR));
            const double hand = angle(skeleton_turn(anim::Bone::LeftHand), body_turn(::saige::Bone::ForearmL)), head = angle(skeleton_turn(anim::Bone::Head), body_turn(::saige::Bone::Head));
            const double neck = angle(skeleton_turn(anim::Bone::Neck), render::slerp(body_turn(::saige::Bone::Torso), body_turn(::saige::Bone::Head), 0.5f));
            std::printf("  joint turns against body turns (radians): feet %.4f %.4f, hand %.4f, head %.4f, neck %.4f\n", foot, rfoot, hand, head, neck);
            require(foot < 0.02 && rfoot < 0.02 && hand < 0.02 && head < 0.02 && neck < 0.02, "feet, hands, head and neck turn as their bodies do (within a degree)");
        }
        // Bone lengths are the rig's.
        const auto rest_joints = joint_world(b.skin, rest);
        double length_error = 0.0;
        for (std::size_t j = 0; j < b.skin.parent.size(); ++j) {
            if (b.skin.parent[j] < 0) continue;
            const auto p = static_cast<std::size_t>(b.skin.parent[j]);
            const float lr = render::length(rest_joints[j].position - rest_joints[p].position), lf = render::length(world_joints[j].position - world_joints[p].position);
            length_error = std::max(length_error, static_cast<double>(std::fabs(lr - lf)));
        }
        std::printf("  bone lengths against the rig after the collapse: largest difference %.2e m\n", length_error);
        require(length_error < 1e-4, "bone lengths are exactly the rig's (the skeleton follows by rotations)");
        // Moving the whole thing: a different frame gives the same pose in the model space.
        require(spec_from_pose(b.skin, b.map, rest, 75.0, {{0, 0, 0}, 0.0}).height > 1.5, "a rig without a bone it needs is refused below; this one is whole");
        bool threw = false;
        try {
            anim::HumanoidMap broken = b.map;
            broken.joint[static_cast<std::size_t>(anim::Bone::LeftHand)] = -1;
            (void)spec_from_pose(b.skin, broken, rest, 75.0, frame);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        require(threw, "a rig without a hand is refused");
        std::cout << "ragdoll glue tests passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
