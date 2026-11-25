// SAIGE animation: operations on poses that the stateless sampling in gfx_assets/animation.hpp leaves to the caller: model-space joint transforms, blending, and distance.
#pragma once

#include "saige/gfx_assets/animation.hpp"

#include <vector>

namespace saige::anim {

// A joint's transform in the model space of its skin (top joints include `Skin::base`). Rotation only: the engine's rigs are authored with unit scale, and the functions that use
// these assume it (a skin with a scale far from 1 would need matrices).
struct JointWorld {
    render::Float3 position;
    render::Quat rotation;
};

[[nodiscard]] std::vector<JointWorld> joint_world(const gfx::Skin& skin, const gfx::Pose& pose);

// The world transform of one joint's parent (identity for a top joint, ignoring Skin::base).
[[nodiscard]] JointWorld parent_world(const gfx::Skin& skin, const std::vector<JointWorld>& world, std::size_t joint);

// Sets joint j's local rotation so that its model-space rotation becomes `rotation`, given the model-space transforms `world` of the pose (from joint_world). A top joint's parent is its Skin::base node.
void set_joint_world_rotation(const gfx::Skin& skin, gfx::Pose& pose, const std::vector<JointWorld>& world, std::size_t joint, render::Quat rotation);

// Moves the whole skeleton by `shift` in model space: every top joint's translation (which is in its parent node's space, see Skin::base) is changed by the shift brought through the transpose of that
// node's rotation.
void translate_model(const gfx::Skin& skin, render::Float3 shift, gfx::Pose& pose);

// dst <- dst * (1 - t) + src * t, joint by joint: translation and scale linearly, rotation by the shortest-arc slerp. Both poses must have the same joint count. t is clamped to [0, 1].
void blend_into(gfx::Pose& dst, const gfx::Pose& src, float t);
[[nodiscard]] gfx::Pose blend_poses(const gfx::Pose& a, const gfx::Pose& b, float t);

// The largest rotation difference (radians, 0..pi) over the joints of two poses with the same joint count; the largest translation difference (meters) over joints goes to `max_translation`.
[[nodiscard]] float pose_distance(const gfx::Pose& a, const gfx::Pose& b, float* max_translation = nullptr);

// The rotation angle (radians, 0..pi) taking `a` to `b`.
[[nodiscard]] float rotation_angle_between(render::Quat a, render::Quat b);

} // namespace saige::anim
