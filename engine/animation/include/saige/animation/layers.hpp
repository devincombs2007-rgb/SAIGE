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

#include <algorithm>
#include <cmath>
#include <stdexcept>
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

// Standing and crouching are two locomotion sets (UAL: `Idle_Loop` and the walk cycles against `Crouch_Idle_Loop` and `Crouch_Fwd_Loop`), each with its own controller; `StanceBlend` is the ramp that
// crossfades their poses (`blend_into(standing, crouched, weight())`) when the capsule's `crouched` flag changes, over `fade` seconds, never faster, so the body does not pop between stances. The
// controllers keep running both while the weight is strictly between 0 and 1; a host may skip the one that has no weight.
class StanceBlend {
public:
    explicit StanceBlend(float fade = 0.20f) : fade_(fade > 0.0f ? fade : 1e-4f) {}
    void update(float dt, bool crouched) {
        if (!(dt > 0.0f) || !std::isfinite(dt)) throw std::invalid_argument("StanceBlend::update: dt must be finite and positive");
        const float step = dt / fade_;
        weight_ = crouched ? std::min(1.0f, weight_ + step) : std::max(0.0f, weight_ - step);
    }
    [[nodiscard]] float weight() const noexcept { return weight_; }
    // The same ramp eased (smoothstep): it leaves a stance slowly, passes the middle at the same time as the linear one and arrives slowly, so neither the start nor the end of a crouch shows as a kink. Use it for
    // the pose blend; `weight()` stays the linear progress (and what a host compares with 0 and 1).
    [[nodiscard]] float eased() const noexcept { return weight_ * weight_ * (3.0f - 2.0f * weight_); }
    [[nodiscard]] bool settled() const noexcept { return weight_ <= 0.0f || weight_ >= 1.0f; }

private:
    float fade_;
    float weight_ = 0.0f;
};

// The capsule height that matches a crouched pose: the highest the head joint gets in the crouch clips plus what the standing character has above its head joint (the capsule's top minus the standing
// pose's head height), so the animation never rises above the capsule that is let under a low ceiling. Clamped to [minimum, standing_height].
[[nodiscard]] inline float crouch_capsule_height(float crouched_head_max, float standing_head, float standing_height, float minimum) {
    const float above_head = standing_height - standing_head;
    return std::clamp(crouched_head_max + above_head, minimum, standing_height);
}

// An additive layer: what `layer` does relative to `reference` (a flinch, breathing, a lean: the layer's pose against the pose it was authored from, usually the clip's first frame or the rest pose) is
// applied on top of whatever `dst` already does, by weight * mask. The rotation delta is conj(reference) * layer in each joint's local space, scaled by slerping from the identity, and translations add
// the difference. A layer equal to its reference changes nothing, and a joint whose mask is 0 is not touched. Throws std::invalid_argument for poses or a mask of different sizes.
void blend_additive(gfx::Pose& dst, const gfx::Pose& layer, const gfx::Pose& reference, const BoneMask& mask, float weight);

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
