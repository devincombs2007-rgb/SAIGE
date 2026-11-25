// SAIGE animation: a BVH (Biovision Hierarchy) importer, the plain-text motion capture format used by the CMU Graphics Lab database (free for research and commercial use) and many others.
//
// The result is the same data the glTF importer produces, so everything downstream (sampling, blending, analysis, retargeting) treats a mocap clip like any other. The skeleton and the clip are
// converted from the file's frame to the engine's: BVH files of the CMU conversion are Y-up and face +Z, which is glTF's convention, so (x, y, z) becomes (z, x, y) exactly as `gfx::gltf_to_engine`.
// Units are scaled by `unit_scale` to meters.
//
// A mocap file records a person walking in a straight line, not a loop: `extract_loop` finds a stretch that repeats and makes a seamless in-place cycle out of it.
#pragma once

#include "saige/animation/clip_analysis.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace saige::anim {

struct BvhOptions {
    float unit_scale = 1.0f;          // meters per file unit (the CMU conversion: 0.056444, i.e. 1 unit = 1 / 0.45 inch)
    bool first_frame_is_rest = true;  // the CMU conversion inserts a T-pose as frame 0: it becomes the skeleton's rest pose and is not part of the clip
    std::size_t max_frames = 200000;
    std::size_t max_joints = 512;
};

struct BvhResult {
    std::optional<gfx::Skin> skin;
    std::optional<gfx::AnimationClip> clip;  // channels address the skin's `joint_nodes` (joint i is node i)
    float frame_time = 0.0f;
    std::string error;                       // set when skin is empty
    [[nodiscard]] bool ok() const noexcept { return skin.has_value() && clip.has_value(); }
};

[[nodiscard]] BvhResult parse_bvh(std::string_view text, const BvhOptions& options = {});

// The CMU conversion's scale (meters per unit).
inline constexpr float cmu_unit_scale = 0.056444f;

struct LoopOptions {
    float min_duration = 0.45f;   // seconds: the shortest cycle considered (a run is about 0.7 s, a walk about 1.1 s)
    float max_duration = 1.8f;
    float max_heading_drift_deg = 6.0f;  // the stretch must be nearly straight
    float blend_window = 0.10f;   // seconds of crossfade used to close the seam
};

struct LoopResult {
    std::optional<gfx::AnimationClip> clip;  // an in-place cycle (no net horizontal travel); its duration is the cycle
    float source_start = 0.0f, source_end = 0.0f;  // the stretch of the source it came from
    float travel = 0.0f;       // meters of net travel along the facing direction over the cycle in the source (so travel / duration is the speed the actor moved at)
    float seam_error = 0.0f;   // radians: the largest joint rotation difference between the first and last pose before closing the seam
    std::string error;
};

// Finds the best repeating stretch of `clip` (the pair of times, a cycle apart and in the same foot phase, whose poses differ least), and returns it as an in-place seamless loop sampled at `fps`.
[[nodiscard]] LoopResult extract_loop(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, const gfx::AnimationClip& clip, const LoopOptions& options = {}, float fps = 60.0f);

} // namespace saige::anim
