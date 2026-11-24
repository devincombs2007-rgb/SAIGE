// Synthetic fixtures for the animation tests: a procedural humanoid skeleton and a gait clip whose feet follow exact analytic paths, so a test knows the true ground speed, stride and foot phase and can
// check what the code measures against them. Everything is built in code, so these tests need no asset files.
#pragma once

#include "saige/animation/humanoid.hpp"
#include "saige/animation/ik.hpp"
#include "saige/animation/locomotion.hpp"

#include <cmath>
#include <string>
#include <vector>

namespace fixtures {

using namespace saige;
using render::Float3;
using render::Quat;

struct Biped {
    gfx::Skin skin;
    anim::HumanoidMap map;
    anim::RigInfo rig;
};

enum class Naming { Unreal, Mixamo };

// A standing humanoid facing +X (rotated about +Z by `yaw` if given), legs scaled by `leg_scale`. Joints are ordered parents first.
inline Biped make_biped(float leg_scale = 1.0f, Naming naming = Naming::Unreal, float yaw = 0.0f, bool a_pose = false) {
    struct J {
        const char* unreal;
        const char* mixamo;
        int parent;
        Float3 offset;
    };
    const float L = leg_scale;
    const float arm_down = a_pose ? -0.2f : 0.0f;  // an A-pose lowers the arms
    const J joints[] = {
        {"root", "mixamorig:Root", -1, {0, 0, 0}},
        {"pelvis", "mixamorig:Hips", 0, {0, 0, 0.12f + 0.83f * L}},  // so the toes of the rest pose are on the floor at any leg scale
        {"spine_01", "mixamorig:Spine", 1, {0, 0, 0.10f}},
        {"spine_02", "mixamorig:Spine1", 2, {0, 0, 0.15f}},
        {"spine_03", "mixamorig:Spine2", 3, {0, 0, 0.15f}},
        {"neck_01", "mixamorig:Neck", 4, {0, 0, 0.20f}},
        {"Head", "mixamorig:Head", 5, {0, 0, 0.10f}},
        {"clavicle_l", "mixamorig:LeftShoulder", 4, {0, 0.05f, 0.15f}},
        {"upperarm_l", "mixamorig:LeftArm", 7, {0, 0.12f, arm_down * 0.5f}},
        {"lowerarm_l", "mixamorig:LeftForeArm", 8, {0, 0.28f, arm_down}},
        {"hand_l", "mixamorig:LeftHand", 9, {0, 0.26f, arm_down}},
        {"clavicle_r", "mixamorig:RightShoulder", 4, {0, -0.05f, 0.15f}},
        {"upperarm_r", "mixamorig:RightArm", 11, {0, -0.12f, arm_down * 0.5f}},
        {"lowerarm_r", "mixamorig:RightForeArm", 12, {0, -0.28f, arm_down}},
        {"hand_r", "mixamorig:RightHand", 13, {0, -0.26f, arm_down}},
        {"thigh_l", "mixamorig:LeftUpLeg", 1, {0, 0.09f, -0.05f}},
        {"calf_l", "mixamorig:LeftLeg", 15, {0, 0, -0.43f * L}},
        {"foot_l", "mixamorig:LeftFoot", 16, {0, 0, -0.40f * L}},
        {"ball_l", "mixamorig:LeftToeBase", 17, {0.12f, 0, -0.07f}},
        {"thigh_r", "mixamorig:RightUpLeg", 1, {0, -0.09f, -0.05f}},
        {"calf_r", "mixamorig:RightLeg", 19, {0, 0, -0.43f * L}},
        {"foot_r", "mixamorig:RightFoot", 20, {0, 0, -0.40f * L}},
        {"ball_r", "mixamorig:RightToeBase", 21, {0.12f, 0, -0.07f}},
    };
    const Quat facing = render::from_axis_angle({0, 0, 1}, yaw);
    Biped b;
    std::vector<Float3> global;
    for (std::size_t i = 0; i < sizeof(joints) / sizeof(joints[0]); ++i) {
        const J& j = joints[i];
        b.skin.joint_nodes.push_back(static_cast<std::int32_t>(i));
        b.skin.joint_names.push_back(naming == Naming::Unreal ? j.unreal : j.mixamo);
        b.skin.parent.push_back(j.parent);
        b.skin.base.push_back(render::Mat4::identity());
        render::Transform t;
        t.position = j.parent < 0 ? Float3{0, 0, 0} : j.offset;  // the root carries the heading; everything below inherits it
        if (j.parent < 0) t.rotation = facing;  // the model faces `yaw` about the root; everything below inherits it
        b.skin.rest.push_back(t);
    }
    // Inverse binds from the global rest transforms.
    std::vector<render::Mat4> g(b.skin.joint_count(), render::Mat4::identity());
    for (std::size_t i = 0; i < b.skin.joint_count(); ++i) {
        const render::Mat4 local = render::to_matrix(b.skin.rest[i]);
        g[i] = b.skin.parent[i] >= 0 ? g[static_cast<std::size_t>(b.skin.parent[i])] * local : local;
        render::Mat4 inv = render::Mat4::identity();
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) inv.at(r, c) = g[i].at(c, r);
        }
        const Float3 p{g[i].at(0, 3), g[i].at(1, 3), g[i].at(2, 3)};
        for (int r = 0; r < 3; ++r) inv.at(r, 3) = -(inv.at(r, 0) * p.x + inv.at(r, 1) * p.y + inv.at(r, 2) * p.z);
        b.skin.inverse_bind.push_back(inv);
    }
    b.map = *anim::map_humanoid(b.skin);
    b.rig = anim::analyze_rig(b.skin, b.map);
    return b;
}

// A walking cycle in place: the contact point of each foot (the ball) moves back at exactly `speed` while planted (a fraction `stance` of the cycle), then swings forward in an arc; the pelvis bobs.
// Legs are solved with the two-bone IK. `root_drift`: also carry the root forward at `speed` (a root-motion clip).
inline gfx::AnimationClip make_gait(const Biped& b, float speed, float cycle, float stance = 0.6f, float step_height = 0.10f, bool root_drift = false, int samples = 120, float left_offset = 0.0f) {
    const gfx::Skin& skin = b.skin;
    const std::size_t pelvis = static_cast<std::size_t>(b.map[anim::Bone::Hips]);
    const render::Float3 fwd = b.rig.forward, left = b.rig.left;
    const float half = 0.5f * speed * stance * cycle;  // half the planted travel
    // A longer stride needs more bent knees: lower the pelvis so that the ankle at the stride's ends is within reach of the leg (a straight leg cannot reach forward at all).
    const float crouch = std::clamp(0.6f * half, 0.08f, 0.30f);
    gfx::AnimationClip clip;
    clip.name = "gait";
    clip.duration = cycle;
    struct Track {
        std::size_t joint;
        gfx::AnimationSampler sampler;
    };
    const anim::Bone chain[2][4] = {{anim::Bone::LeftUpperLeg, anim::Bone::LeftLowerLeg, anim::Bone::LeftFoot, anim::Bone::LeftToes}, {anim::Bone::RightUpperLeg, anim::Bone::RightLowerLeg, anim::Bone::RightFoot, anim::Bone::RightToes}};
    std::vector<gfx::AnimationSampler> rot(skin.joint_count());
    for (auto& s : rot) s.components = 4;
    gfx::AnimationSampler hips_pos, root_pos;
    hips_pos.components = 3;
    root_pos.components = 3;
    const gfx::Pose rest = gfx::rest_pose(skin);
    const std::vector<anim::JointWorld> rest_w = anim::joint_world(skin, rest);
    const float toe_dx = 0.12f, toe_dz = -0.07f;
    const float hips_rest_z = rest_w[pelvis].position.z;
    for (int k = 0; k <= samples; ++k) {
        const float t = cycle * static_cast<float>(k) / static_cast<float>(samples);
        gfx::Pose pose = rest;
        const float bob = 0.02f * std::cos(4.0f * 3.14159265f * t / cycle) - crouch;
        const Float3 hips_world = rest_w[pelvis].position + Float3{0, 0, bob};
        // Hips: the local position under the root (root has the heading rotation).
        const anim::JointWorld rootw = rest_w[static_cast<std::size_t>(skin.parent[pelvis])];
        pose.local[pelvis].position = render::rotate(render::conjugate(rootw.rotation), hips_world - rootw.position);
        std::vector<anim::JointWorld> w = anim::joint_world(skin, pose);
        for (int f = 0; f < 2; ++f) {
            const float p = std::fmod(t / cycle + (f == 0 ? left_offset : 0.5f + left_offset) + 1.0f, 1.0f);  // phase of this foot: planted for p < stance
            float x, z;
            if (p < stance) {
                x = half - (2.0f * half) * (p / stance);
                z = 0.0f;
            } else {
                const float u = (p - stance) / (1.0f - stance);
                x = -half + (2.0f * half) * (0.5f - 0.5f * std::cos(3.14159265f * u));
                z = step_height * std::sin(3.14159265f * u);
            }
            const float side = f == 0 ? 0.09f : -0.09f;
            const Float3 toe = fwd * x + left * side + Float3{0, 0, z};
            // The ankle is the toe minus the foot's fixed offset (the foot stays flat, facing forward).
            const Float3 ankle = toe - fwd * toe_dx - Float3{0, 0, toe_dz};
            const std::size_t up = static_cast<std::size_t>(b.map[chain[f][0]]), lo = static_cast<std::size_t>(b.map[chain[f][1]]), ft = static_cast<std::size_t>(b.map[chain[f][2]]);
            anim::solve_two_bone(skin, pose, up, lo, ft, ankle, fwd);
            w = anim::joint_world(skin, pose);
            // Keep the foot flat: its world rotation is the heading.
            const Quat parent = w[lo].rotation;
            pose.local[ft].rotation = render::normalize(render::conjugate(parent) * rest_w[ft].rotation);
            w = anim::joint_world(skin, pose);
        }
        for (const std::size_t j : {pelvis}) {
            rot[j].times.push_back(t);
            rot[j].values.insert(rot[j].values.end(), {pose.local[j].rotation.x, pose.local[j].rotation.y, pose.local[j].rotation.z, pose.local[j].rotation.w});
        }
        for (int f = 0; f < 2; ++f) {
            for (int c = 0; c < 3; ++c) {
                const std::size_t j = static_cast<std::size_t>(b.map[chain[f][c]]);
                rot[j].times.push_back(t);
                rot[j].values.insert(rot[j].values.end(), {pose.local[j].rotation.x, pose.local[j].rotation.y, pose.local[j].rotation.z, pose.local[j].rotation.w});
            }
        }
        hips_pos.times.push_back(t);
        hips_pos.values.insert(hips_pos.values.end(), {pose.local[pelvis].position.x, pose.local[pelvis].position.y, pose.local[pelvis].position.z});
        root_pos.times.push_back(t);
        const Float3 drift = root_drift ? fwd * (speed * t) : Float3{0, 0, 0};
        root_pos.values.insert(root_pos.values.end(), {drift.x, drift.y, drift.z});
    }
    (void)hips_rest_z;
    const auto add = [&](std::size_t joint, gfx::AnimationPath path, gfx::AnimationSampler s) {
        clip.samplers.push_back(std::move(s));
        clip.channels.push_back({skin.joint_nodes[joint], path, static_cast<std::int32_t>(clip.samplers.size() - 1)});
    };
    add(pelvis, gfx::AnimationPath::Translation, hips_pos);
    if (root_drift) add(static_cast<std::size_t>(skin.parent[pelvis]), gfx::AnimationPath::Translation, root_pos);
    for (std::size_t j = 0; j < rot.size(); ++j) {
        if (!rot[j].times.empty()) add(j, gfx::AnimationPath::Rotation, rot[j]);
    }
    return clip;
}

// A one-shot or looping clip that holds the rest pose with the pelvis at a height offset and the arms raised by `lift` (a visible, distinct pose per clip).
inline gfx::AnimationClip make_pose_clip(const Biped& b, float duration, float pelvis_drop, float arm_lift) {
    const std::size_t pelvis = static_cast<std::size_t>(b.map[anim::Bone::Hips]);
    gfx::AnimationClip clip;
    clip.name = "pose";
    clip.duration = duration;
    gfx::AnimationSampler pos;
    pos.components = 3;
    pos.times = {0.0f, duration};
    const render::Transform& r = b.skin.rest[pelvis];
    for (int i = 0; i < 2; ++i) pos.values.insert(pos.values.end(), {r.position.x, r.position.y, r.position.z - pelvis_drop * (i == 0 ? 0.0f : 1.0f)});
    clip.samplers.push_back(pos);
    clip.channels.push_back({b.skin.joint_nodes[pelvis], gfx::AnimationPath::Translation, 0});
    for (const anim::Bone arm : {anim::Bone::LeftUpperArm, anim::Bone::RightUpperArm}) {
        const std::size_t j = static_cast<std::size_t>(b.map[arm]);
        gfx::AnimationSampler s;
        s.components = 4;
        s.times = {0.0f, duration};
        const Quat q0 = b.skin.rest[j].rotation;
        const Quat q1 = render::normalize(render::from_axis_angle({1, 0, 0}, arm == anim::Bone::LeftUpperArm ? arm_lift : -arm_lift) * q0);
        s.values = {q0.x, q0.y, q0.z, q0.w, q1.x, q1.y, q1.z, q1.w};
        clip.samplers.push_back(s);
        clip.channels.push_back({b.skin.joint_nodes[j], gfx::AnimationPath::Rotation, static_cast<std::int32_t>(clip.samplers.size() - 1)});
    }
    return clip;
}

} // namespace fixtures
