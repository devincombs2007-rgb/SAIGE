// SAIGE animation: retargeting a clip from one humanoid skeleton onto another with different proportions, rest pose and joint layout.
//
// The method works in world-orientation space. For each humanoid bone present in both rigs, the rotation the source bone makes from its rest pose, expressed in the model's frame, is applied to the
// target bone's rest orientation; local rotations then follow from the target's own hierarchy, so unmapped joints (fingers, twist bones, extra spine) keep their rest local pose and follow their
// parent. This is exact when both rigs face the same way (any difference in facing is corrected by a yaw about +Z) and is robust to T-pose versus A-pose rests, because only the change from rest is
// carried over. The hips' translation is scaled by the ratio of the rigs' leg lengths, so a retargeted walk keeps its stride in proportion and its feet keep planted (the speed it needs is the source's
// speed times the same ratio).
//
// What it does not do: it does not solve contacts. Residual foot sliding and floor penetration from proportion differences are the job of foot IK (ik.hpp).
#pragma once

#include "saige/animation/clip_analysis.hpp"

namespace saige::anim {

struct RetargetOptions {
    float fps = 60.0f;            // the baked clip's sampling rate
    bool scale_translation = true;
};

struct RetargetSource {
    const gfx::Skin* skin;
    const HumanoidMap* map;
    const RigInfo* rig;
    const gfx::Pose* rest;        // the pose the source's "rest" means for retargeting (the skin's rest, or the T-pose frame of a mocap file)
};

// Bakes `clip` (sampled on the source skin) into a clip on `target` (channels address target.joint_nodes). Throws std::invalid_argument for a rig that cannot be mapped.
[[nodiscard]] gfx::AnimationClip retarget_clip(const RetargetSource& source, const gfx::AnimationClip& clip, const gfx::Skin& target, const HumanoidMap& target_map, const RigInfo& target_rig,
                                               const RetargetOptions& options = {});

} // namespace saige::anim
