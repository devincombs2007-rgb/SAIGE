#include "saige/animation/layers.hpp"

#include "saige/animation/pose_ops.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::anim {

using render::Float3;
using render::Quat;

BoneMask mask_below(const gfx::Skin& skin, const std::vector<std::int32_t>& roots, float weight) {
    BoneMask mask;
    mask.weight.assign(skin.joint_count(), 0.0f);
    const float w = std::clamp(weight, 0.0f, 1.0f);
    // Joints are not required to be ordered parents first, so walk up from every joint.
    for (std::size_t j = 0; j < skin.joint_count(); ++j) {
        std::int32_t at = static_cast<std::int32_t>(j);
        for (std::size_t guard = 0; at >= 0 && guard <= skin.joint_count(); ++guard) {
            if (std::find(roots.begin(), roots.end(), at) != roots.end()) {
                mask.weight[j] = w;
                break;
            }
            at = skin.parent[static_cast<std::size_t>(at)];
        }
    }
    return mask;
}

BoneMask upper_body_mask(const gfx::Skin& skin, const HumanoidMap& map, float spine_weight) {
    BoneMask mask = mask_below(skin, {map[Bone::Spine]}, 1.0f);
    if (map.has(Bone::Spine)) mask.weight[static_cast<std::size_t>(map[Bone::Spine])] = std::clamp(spine_weight, 0.0f, 1.0f);
    return mask;
}

void blend_masked(gfx::Pose& dst, const gfx::Pose& src, const BoneMask& mask, float weight) {
    if (dst.local.size() != src.local.size() || mask.weight.size() != dst.local.size()) throw std::invalid_argument("blend_masked: the poses and the mask must have the same joint count");
    const float w = std::clamp(weight, 0.0f, 1.0f);
    if (!(w > 0.0f)) return;
    for (std::size_t j = 0; j < dst.local.size(); ++j) {
        const float t = w * std::clamp(mask.weight[j], 0.0f, 1.0f);
        if (!(t > 0.0f)) continue;
        render::Transform& a = dst.local[j];
        const render::Transform& b = src.local[j];
        a.position = a.position + (b.position - a.position) * t;
        a.scale = a.scale + (b.scale - a.scale) * t;
        a.rotation = render::slerp(a.rotation, b.rotation, t);
    }
}

namespace {

float yaw_of(Float3 d, const RigInfo& rig) { return std::atan2(render::dot(d, rig.left), render::dot(d, rig.forward)); }
float pitch_of(Float3 d, const RigInfo& rig) {
    const float horizontal = std::hypot(render::dot(d, rig.left), render::dot(d, rig.forward));
    return std::atan2(d.z, horizontal);
}
Float3 direction_from(float yaw, float pitch, const RigInfo& rig) {
    const float c = std::cos(pitch);
    return rig.forward * (c * std::cos(yaw)) + rig.left * (c * std::sin(yaw)) + Float3{0, 0, std::sin(pitch)};
}

} // namespace

float look_at(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, gfx::Pose& pose, const render::Transform& root, Float3 target, const LookAtConfig& cfg, LookAtState& state, float weight,
              float dt) {
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) throw std::invalid_argument("look_at: the target must be finite");
    if (!(dt > 0.0f) || !std::isfinite(dt)) throw std::invalid_argument("look_at: dt must be finite and positive");
    if (!map.has(Bone::Head)) return 0.0f;
    const std::size_t head = static_cast<std::size_t>(map[Bone::Head]);

    // The target in the rig's model space, and the direction to it from the head.
    const Float3 target_model = render::rotate(render::conjugate(root.rotation), target - root.position);
    std::vector<JointWorld> world = joint_world(skin, pose);
    Float3 to_target = target_model - world[head].position;
    const float distance = render::length(to_target);
    if (!(distance > 1e-4f)) return 0.0f;  // the target is at the head: nothing to look at
    to_target = to_target * (1.0f / distance);

    // The wanted look in the character's frame, within the limits, smoothed.
    const float want_yaw = std::clamp(yaw_of(to_target, rig), -cfg.max_yaw, cfg.max_yaw);
    const float want_pitch = std::clamp(pitch_of(to_target, rig), -cfg.max_pitch_down, cfg.max_pitch_up);
    if (!state.initialized) {
        state.yaw = want_yaw;
        state.pitch = want_pitch;
        state.initialized = true;
    } else {
        const float k = cfg.smoothing > 0.0f ? 1.0f - std::exp(-dt / cfg.smoothing) : 1.0f;
        state.yaw += (want_yaw - state.yaw) * k;
        state.pitch += (want_pitch - state.pitch) * k;
    }
    const float w = std::clamp(weight, 0.0f, 1.0f);
    if (!(w > 0.0f)) return 0.0f;

    // Where the head faces now: its rest-pose facing (the character's forward) carried by its current world rotation.
    const std::vector<JointWorld> rest_world = joint_world(skin, gfx::rest_pose(skin));
    const Float3 head_local_forward = render::rotate(render::conjugate(rest_world[head].rotation), rig.forward);
    const Float3 current = render::rotate(world[head].rotation, head_local_forward);
    const Float3 wanted = direction_from(state.yaw, state.pitch, rig);
    const Float3 axis = render::cross(current, wanted);
    const float axis_len = render::length(axis);
    const float angle = std::atan2(axis_len, render::dot(current, wanted));
    if (axis_len > 1e-6f && angle > 1e-6f) {
        struct Link {
            Bone bone;
            float share;
        };
        const Link chain[4] = {{Bone::Spine, cfg.spine_share}, {Bone::Chest, cfg.chest_share}, {Bone::Neck, cfg.neck_share}, {Bone::Head, cfg.head_share}};
        float total = 0.0f;
        for (const Link& l : chain) total += map.has(l.bone) ? std::max(l.share, 0.0f) : 0.0f;
        if (total > 0.0f) {
            const Float3 unit_axis = axis * (1.0f / axis_len);
            for (const Link& l : chain) {
                if (!map.has(l.bone) || !(l.share > 0.0f)) continue;
                const std::size_t j = static_cast<std::size_t>(map[l.bone]);
                world = joint_world(skin, pose);  // the joints above this one have just turned
                const Quat delta = render::from_axis_angle(unit_axis, angle * (l.share / total) * w);
                const JointWorld parent = parent_world(skin, world, j);
                pose.local[j].rotation = render::normalize(render::conjugate(parent.rotation) * delta * world[j].rotation);
            }
        }
    }
    // How far the head still is from facing the target.
    world = joint_world(skin, pose);
    const Float3 after = render::rotate(world[head].rotation, head_local_forward);
    return std::atan2(render::length(render::cross(after, to_target)), render::dot(after, to_target));
}

} // namespace saige::anim
