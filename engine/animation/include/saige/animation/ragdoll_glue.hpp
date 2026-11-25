// SAIGE animation: a skeleton to and from the humanoid ragdoll (include/saige/ragdoll.hpp), the join between animation and physics. The model space of the rigs is SAIGE's (+X forward, +Y left, +Z up), so a
// pose maps to the world by a translation and a turn about +Z (`RagdollFrame`).
//
//   spec_from_pose: the measurements of the body in a pose (where the shoulders, elbows, hips, knees ankles and toes are), from the bone map; with `humanoid_body_pose` it gives the thirteen body transforms of that
//     pose, which is what `Ragdoll::start` takes (and what a Ragdoll is built from: build it from the rest pose, start it from the live one).
//   pose_from_ragdoll: the skeleton that follows the bodies. Each joint is turned by the rotation its body has turned since the reference pose (the pose the ragdoll was started from, as bodies), parents before
//     children, and the hips are moved to the pelvis, so bone lengths are exactly the rig's: the ragdoll drives the skeleton by rotations.
#pragma once

#include "saige/animation/humanoid.hpp"
#include "saige/animation/pose_ops.hpp"
#include "saige/ragdoll.hpp"

#include <cmath>
#include <stdexcept>

namespace saige::anim {

struct RagdollFrame {
    DVec origin{};     // world position of the model space's origin
    double yaw = 0.0;  // radians about +Z
};

namespace detail {

inline DVec to_world(const RagdollFrame& f, render::Float3 p) {
    const double c = std::cos(f.yaw), s = std::sin(f.yaw);
    const double px = static_cast<double>(p.x), py = static_cast<double>(p.y), pz = static_cast<double>(p.z);
    return {f.origin.x + c * px - s * py, f.origin.y + s * px + c * py, f.origin.z + pz};
}
inline render::Float3 to_model(const RagdollFrame& f, DVec p) {
    const double c = std::cos(-f.yaw), s = std::sin(-f.yaw);
    const double x = p.x - f.origin.x, y = p.y - f.origin.y;
    return {static_cast<float>(c * x - s * y), static_cast<float>(s * x + c * y), static_cast<float>(p.z - f.origin.z)};
}
inline render::Quat yaw_quat(double yaw) { return render::from_axis_angle({0, 0, 1}, static_cast<float>(yaw)); }
inline DQuat to_dquat(render::Quat q) { return {q.x, q.y, q.z, q.w}; }
inline render::Quat to_quat(DQuat q) { return {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w)}; }

} // namespace detail

// The body measurements of `pose` (joints in the world through `frame`). Throws std::invalid_argument when the rig lacks a bone the ragdoll needs (both arms and legs, a head and a neck or chest).
inline HumanoidSpec spec_from_pose(const gfx::Skin& skin, const HumanoidMap& map, const gfx::Pose& pose, double mass, const RagdollFrame& frame) {
    for (const Bone b : {Bone::Hips, Bone::Head, Bone::LeftUpperArm, Bone::LeftLowerArm, Bone::LeftHand, Bone::RightUpperArm, Bone::RightLowerArm, Bone::RightHand, Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot,
                         Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot}) {
        if (!map.has(b)) throw std::invalid_argument(std::string("spec_from_pose: the rig has no ") + bone_name(b));
    }
    if (!map.has(Bone::Neck) && !map.has(Bone::Chest)) throw std::invalid_argument("spec_from_pose: the rig has neither a neck nor a chest");
    const auto world = joint_world(skin, pose);
    const auto at = [&](Bone b) { return detail::to_world(frame, world[static_cast<std::size_t>(map[b])].position); };
    const DVec hips = at(Bone::Hips);
    const DVec neck = map.has(Bone::Neck) ? at(Bone::Neck) : at(Bone::Chest);
    const DVec head_joint = at(Bone::Head);
    double floor_z = 1e9;
    for (const Bone b : {Bone::LeftFoot, Bone::RightFoot}) floor_z = std::min(floor_z, at(b).z);
    for (const Bone b : {Bone::LeftToes, Bone::RightToes}) {
        if (map.has(b)) floor_z = std::min(floor_z, at(b).z);
    }
    const double height = std::max(0.5, (neck.z - floor_z) / 0.85);
    const double c = std::cos(frame.yaw), s = std::sin(frame.yaw);
    HumanoidSpec spec = standard_humanoid(height, mass, {0, 0, 0}, {c, s, 0});
    spec.pelvis = hips;
    spec.chest = {hips.x + 0.58 * (neck.x - hips.x), hips.y + 0.58 * (neck.y - hips.y), hips.z + 0.58 * (neck.z - hips.z)};
    spec.neck = neck;
    spec.head = {head_joint.x + 0.5 * (head_joint.x - neck.x), head_joint.y + 0.5 * (head_joint.y - neck.y), head_joint.z + 0.5 * (head_joint.z - neck.z)};
    for (int i = 0; i < 2; ++i) {
        const bool left = i == 0;
        spec.shoulder[i] = at(left ? Bone::LeftUpperArm : Bone::RightUpperArm);
        spec.elbow[i] = at(left ? Bone::LeftLowerArm : Bone::RightLowerArm);
        spec.wrist[i] = at(left ? Bone::LeftHand : Bone::RightHand);
        spec.hip[i] = at(left ? Bone::LeftUpperLeg : Bone::RightUpperLeg);
        spec.knee[i] = at(left ? Bone::LeftLowerLeg : Bone::RightLowerLeg);
        spec.ankle[i] = at(left ? Bone::LeftFoot : Bone::RightFoot);
        const Bone toes = left ? Bone::LeftToes : Bone::RightToes;
        spec.toe[i] = map.has(toes) ? at(toes) : DVec{spec.ankle[i].x + c * 0.12 * height, spec.ankle[i].y + s * 0.12 * height, floor_z};
    }
    return spec;
}

// Turns the skeleton's joints to follow the bodies: for every joint with a body, its model-space rotation becomes (the body's turn since `reference`, in the model's axes) times its rotation in `pose`. Joints without a
// body take the turn of the nearest body above them (clavicles and spine joints the torso's, the neck the middle of the torso's and the head's, hands the forearm's, toes the foot's). The hips are then moved to the
// pelvis. `pose` is changed; `reference` must be `humanoid_body_pose` of this same pose and frame.
inline void pose_from_ragdoll(const gfx::Skin& skin, const HumanoidMap& map, gfx::Pose& pose, const RagdollPose& physical, const RagdollPose& reference, const RagdollFrame& frame) {
    struct Link {
        Bone joint;
        ::saige::Bone body;
        bool half_with_head;
    };
    using B = ::saige::Bone;
    // Parents first: the order of the humanoid roles puts every joint after the one it hangs from.
    static constexpr Link links[] = {
        {Bone::Hips, B::Pelvis, false},        {Bone::Spine, B::Torso, false},         {Bone::Chest, B::Torso, false},          {Bone::Neck, B::Torso, true},           {Bone::Head, B::Head, false},
        {Bone::LeftShoulder, B::Torso, false}, {Bone::LeftUpperArm, B::UpperArmL, false}, {Bone::LeftLowerArm, B::ForearmL, false}, {Bone::LeftHand, B::ForearmL, false},
        {Bone::RightShoulder, B::Torso, false}, {Bone::RightUpperArm, B::UpperArmR, false}, {Bone::RightLowerArm, B::ForearmR, false}, {Bone::RightHand, B::ForearmR, false},
        {Bone::LeftUpperLeg, B::ThighL, false}, {Bone::LeftLowerLeg, B::ShinL, false}, {Bone::LeftFoot, B::FootL, false},       {Bone::LeftToes, B::FootL, false},
        {Bone::RightUpperLeg, B::ThighR, false}, {Bone::RightLowerLeg, B::ShinR, false}, {Bone::RightFoot, B::FootR, false},    {Bone::RightToes, B::FootR, false},
    };
    const render::Quat inverse_yaw = render::conjugate(detail::yaw_quat(frame.yaw)), yaw = detail::yaw_quat(frame.yaw);
    const auto turn_of = [&](B body) {
        const std::size_t i = static_cast<std::size_t>(body);
        const render::Quat world_turn = render::normalize(detail::to_quat(physical[i].rotation)) * render::conjugate(render::normalize(detail::to_quat(reference[i].rotation)));
        return inverse_yaw * world_turn * yaw;  // the same rotation in the model's axes
    };
    const auto original = joint_world(skin, pose);  // the rotations to turn: not the ones a parent's turn has already changed
    for (const Link& l : links) {
        if (!map.has(l.joint)) continue;
        auto world = joint_world(skin, pose);
        render::Quat turn = turn_of(l.body);
        if (l.half_with_head) turn = render::slerp(turn, turn_of(B::Head), 0.5f);
        const std::size_t j = static_cast<std::size_t>(map[l.joint]);
        set_joint_world_rotation(skin, pose, world, j, turn * original[j].rotation);
    }
    if (map.has(Bone::Hips)) {
        const auto world = joint_world(skin, pose);
        const render::Float3 hips = world[static_cast<std::size_t>(map[Bone::Hips])].position;
        const render::Float3 wanted = detail::to_model(frame, physical[static_cast<std::size_t>(B::Pelvis)].position);
        translate_model(skin, wanted - hips, pose);
    }
}

} // namespace saige::anim
