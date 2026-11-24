// SAIGE gfx_assets — posing a skeleton from an animation clip, skin matrices, and a CPU skinning reference.
//
// Ownership (agreed in principle with Core on #43, to be confirmed on the PR): this module is *stateless sampling*. It turns (clip, skin,
// time) into a pose and a pose into skin matrices, deterministically, with no clock and no playback state. Playback state (current time,
// speed, looping policy), blending between clips, layering and state machines belong to the gameplay layer (Core), which calls these functions
// every tick or frame. GPU skinning consumes the skin matrices this produces.
//
// Everything is engine space: right-handed, +Z up, +X forward, +Y left, meters (see gltf.hpp). A pose stores the local transform of each joint
// of one skin. A joint no channel animates keeps its rest transform.
#pragma once

#include "saige/gfx_assets/gltf.hpp"
#include "saige/render/transform.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace saige::gfx {

struct Pose {
    std::vector<render::Transform> local;  ///< one per joint of the skin
};

/// The rest pose of a skin.
[[nodiscard]] Pose rest_pose(const Skin& skin);

/// Which joint of `skin` each channel of `clip` drives (-1: the channel animates a node that is not a joint of this skin and is skipped).
/// Computed once per (clip, skin) pair.
struct ClipBinding {
    std::vector<int32_t> channel_joint;
};
[[nodiscard]] ClipBinding bind_clip(const AnimationClip& clip, const Skin& skin);

/// Samples one keyframe track at `time` (seconds). Before the first key it holds the first value, after the last the last. Step holds the
/// earlier key; Linear interpolates (a rotation by shortest-path spherical interpolation); CubicSpline is the glTF Hermite spline. The result
/// has `sampler.components` floats (a rotation is normalized).
[[nodiscard]] render::Float4 sample_track(const AnimationSampler& sampler, float time);

/// Evaluates `clip` into `pose` (which must already hold the skin's joint count; start from rest_pose). With `loop`, time wraps over
/// `clip.duration` (a clip of zero duration always samples its first keys); otherwise it clamps. Non-finite times sample at 0.
void evaluate_clip(const AnimationClip& clip, const ClipBinding& binding, float time, bool loop, Pose& pose);

/// The skin matrices of a pose: for each joint, the joint's world transform times its inverse bind matrix, engine space. A vertex bound to
/// joints with weights w_i is placed at sum_i w_i * M_i * p. Throws std::invalid_argument if the pose does not match the skin.
[[nodiscard]] std::vector<render::Mat4> skin_matrices(const Skin& skin, const Pose& pose);

/// The CPU reference for linear blend skinning of one vertex of a skinned primitive (positions only). The GPU path must agree with it.
[[nodiscard]] render::Float3 skin_position(const Primitive& primitive, std::size_t vertex, std::span<const render::Mat4> matrices);

/// Skins a whole primitive's positions (xyz per vertex).
[[nodiscard]] std::vector<float> skin_positions(const Primitive& primitive, std::span<const render::Mat4> matrices);

} // namespace saige::gfx
