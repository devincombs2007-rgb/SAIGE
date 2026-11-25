// SAIGE animation: layering and aiming on top of the locomotion pose.
//
//  * `BoneMask` and `blend_masked`: an animation that drives only part of the body (an aim pose, a carry, a wave) laid over the locomotion pose. `upper_body_mask` selects the spine and everything
//    above it, so the legs, the hips and therefore the feet stay exactly the locomotion's: layering an upper-body clip over a run does not change foot contact at all (the tests compare bit for bit).
//  * `look_at`: turns the spine, chest, neck and head toward a world point, each by its share of the total, within yaw and pitch limits, smoothed in time. Only rotations change, so bone lengths and the
//    legs are untouched. A target beyond the limits is looked at as far as the limits allow; a target behind the character is looked at over the nearer shoulder, to the limit.
//
// Both are stateless about the world (the caller supplies the character's transform and the target); `LookAtState` only holds the smoothed angles.
#pragma once

#include "saige/animation/clip_analysis.hpp"
#include "saige/render/transform.hpp"

#include <vector>

namespace saige::anim {

struct BoneMask {
    std::vector<float> weight;  // per joint of the skin, in [0, 1]
};

// Weight `weight` for every joint at or below any of `roots` (joint indices; -1 entries are ignored), 0 elsewhere.
[[nodiscard]] BoneMask mask_below(const gfx::Skin& skin, const std::vector<std::int32_t>& roots, float weight = 1.0f);
// The spine and everything above it (chest, neck, head, arms, hands): `spine_weight` for the spine joint itself (a value under 1 softens the seam with the legs' animation), 1 above it.
[[nodiscard]] BoneMask upper_body_mask(const gfx::Skin& skin, const HumanoidMap& map, float spine_weight = 1.0f);

// dst <- dst blended toward src, joint by joint, by weight * mask (translation and scale linearly, rotation by the shortest-arc slerp); a joint whose mask is 0 is not touched. The poses and the mask
// must have the skin's joint count (throws std::invalid_argument otherwise). `weight` is clamped to [0, 1].
void blend_masked(gfx::Pose& dst, const gfx::Pose& src, const BoneMask& mask, float weight);

struct LookAtConfig {
    float max_yaw = 1.2217305f;         // radians left or right of the character's facing (70 degrees)
    float max_pitch_up = 0.7853982f;    // 45 degrees
    float max_pitch_down = 0.6108652f;  // 35 degrees
    float smoothing = 0.12f;            // seconds: how quickly the look follows a moving or jumping target
    // The share of the turn each joint takes, base to tip; a rig without a bone drops its share and the rest are normalized.
    float spine_share = 0.15f, chest_share = 0.25f, neck_share = 0.25f, head_share = 0.35f;
};

struct LookAtState {
    float yaw = 0.0f, pitch = 0.0f;  // the smoothed look, radians, in the character's frame (yaw left positive)
    bool initialized = false;
};

// Turns the upper body in `pose` toward `target` (a world position; `root` is the character's world transform with the rig's model space inside it). `weight` in [0, 1] scales the whole effect (0 leaves the
// pose untouched, bit for bit) and dt is the time since the last call. Returns the angle in radians between the head's facing and the direction to the target after the turn, 0 when it looks straight at
// it. Throws std::invalid_argument for a non-finite target or a non-positive or non-finite dt.
float look_at(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, gfx::Pose& pose, const render::Transform& root, render::Float3 target, const LookAtConfig& config, LookAtState& state,
              float weight, float dt);

} // namespace saige::anim
