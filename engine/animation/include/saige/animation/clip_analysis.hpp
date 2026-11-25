// SAIGE animation: what a locomotion clip says about itself, measured from its poses instead of trusted from its name: how fast the ground moves under a planted foot (the speed the character must move
// for the feet not to slide), the stride, when each foot is planted, how well it loops, and which way the rig faces.
#pragma once

#include "saige/animation/humanoid.hpp"
#include "saige/animation/pose_ops.hpp"

namespace saige::anim {

// Which way a rig faces and its proportions, from its rest pose.
struct RigInfo {
    render::Float3 forward{1, 0, 0};  // horizontal unit vector the character faces, in model space (from the legs' lateral axis; +Z is up)
    render::Float3 left{0, 1, 0};     // horizontal unit vector to the character's left
    float hips_height = 0.0f;         // hips above the lowest foot at rest, meters
    float leg_length = 0.0f;          // upper plus lower leg plus the ankle height above the ground, meters
    float contact_offset = 0.0f;      // how high the foot's contact joint (toes, else the ankle) sits above the sole of the foot at rest, meters
};
[[nodiscard]] RigInfo analyze_rig(const gfx::Skin& skin, const HumanoidMap& map);

struct FootCycle {
    float contact_start = 0.0f;  // phase in [0, 1) where the foot becomes planted
    float contact_end = 0.0f;    // phase where it lifts off again (after contact_start, wrapping)
    float planted_fraction = 0.0f;
};

struct ClipInfo {
    bool valid = false;
    float duration = 0.0f;
    float root_speed = 0.0f;       // m/s along the facing direction of the hips' net travel over the clip (0 for an in-place clip)
    float ground_speed = 0.0f;     // m/s the character must travel for planted feet not to slide (from the stance feet, in the in-place version of the clip)
    float stride_length = 0.0f;    // ground_speed * duration: meters per full cycle (two steps)
    float loop_error = 0.0f;       // radians: the largest joint rotation difference between the first and the last pose of a cycle (0 = a perfect loop)
    float hips_height = 0.0f;      // mean height of the hips over the cycle
    render::Float3 drift_velocity{0, 0, 0};  // model-space horizontal velocity of the hips' net travel (the root motion a loop carries), m/s
    FootCycle left, right;
    std::vector<float> left_height, right_height;  // sampled foot heights above the lowest point of the cycle, for tests and plots
};

// Analyses a cycle (a walk, a run) sampled at `samples` evenly spaced poses; `ground_speed`, `stride_length` and the foot cycles are only meaningful for a locomotion clip with planted feet.
[[nodiscard]] ClipInfo analyze_cycle(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, const gfx::AnimationClip& clip, int samples = 120);

// Evaluates the clip in place: the hips' (and every top joint's) horizontal travel is removed linearly over the cycle, so the pose stays near the origin, and the vertical motion is kept. The speed to
// move the character at is ClipInfo::ground_speed.
void evaluate_in_place(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, const gfx::AnimationClip& clip, const gfx::ClipBinding& binding, float time, const ClipInfo& info, gfx::Pose& pose);

} // namespace saige::anim
