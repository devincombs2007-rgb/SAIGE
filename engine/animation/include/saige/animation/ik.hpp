// SAIGE animation: inverse kinematics foundations. Analytic two-bone IK (a leg or an arm), and foot placement on uneven ground built on it.
//
// Both operate on a Pose in place and touch only the rotations of the joints they solve (plus the pelvis height for ground adaptation), so IK layers on top of whatever the blend produced.
// They are stateless and deterministic.
#pragma once

#include "saige/animation/clip_analysis.hpp"

#include <functional>

namespace saige::anim {

struct TwoBoneResult {
    bool solved = false;         // false for a degenerate chain (zero-length bone)
    bool reached = false;        // the target was within reach (otherwise the chain is fully extended toward it)
    float end_error = 0.0f;      // meters between the end joint and the (reachable part of the) target after solving
};

// Solves the chain upper -> lower -> end (joint indices of the skin, `lower` a child of `upper`, `end` a child of `lower`) so that `end` moves to `target` (model space), with the middle joint bending
// toward `pole` (a model-space direction: forward for a knee, backward for an elbow). Only the rotations of `upper` and `lower` change. `weight` in [0, 1] blends the solution with the input pose.
TwoBoneResult solve_two_bone(const gfx::Skin& skin, gfx::Pose& pose, std::size_t upper, std::size_t lower, std::size_t end, render::Float3 target, render::Float3 pole, float weight = 1.0f);

// What the ground is under a point: the height of the surface and its unit normal at a horizontal position near `reference_z` (the query should look for ground around that height, so a floor above or
// below the character is not picked). Returns false when there is none.
struct GroundSample {
    float height = 0.0f;
    render::Float3 normal{0, 0, 1};
};
using GroundQuery = std::function<bool(float x, float y, float reference_z, GroundSample& out)>;

struct FootIkConfig {
    float max_pelvis_drop = 0.35f;    // the pelvis lowers by up to this to let the lower foot reach
    float max_pelvis_rise = 0.10f;
    float plant_height = 0.03f;       // a foot whose contact point is within this above the ground is treated as planted and pinned to the surface
    float blend_height = 0.12f;       // above plant_height + this the foot is left to the animation; in between the correction fades out
    float max_slope_tilt = 0.6f;      // radians the foot may tilt to follow the surface normal
    float pelvis_smoothing = 0.08f;   // seconds
};

struct FootIkState {
    float pelvis_offset = 0.0f;  // the smoothed vertical offset applied to the pelvis last update
};

// Plants the feet of `pose` on uneven ground. `root` is the character's transform in the world (position of its feet origin and heading); `ground` answers in world coordinates. The pose is in the model
// space of the skin: the model origin sits at `root`. Steps: find where each foot's contact point (toes, else the ankle) is in the world, the ground under it, lower the pelvis by the larger deficit
// (clamped, smoothed), then place each planted foot on its surface with two-bone IK and tilt it toward the surface normal. A foot in the air is not touched, so the swing of a gait is preserved.
void adapt_feet_to_ground(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, gfx::Pose& pose, const render::Transform& root, const GroundQuery& ground, const FootIkConfig& config,
                          FootIkState& state, float dt);

} // namespace saige::anim
