// SAIGE animation: root-motion actions (a roll, a lunge, a vault): clips that are meant to carry the character.
//
// Ownership stays with the physics. A locomotion cycle is played in place and follows the speed the character has; an action is the other way round at the start and the same at the end:
//
//   1. `command(dt)` says how fast the character should travel for the next physics step so that it covers what the clip travels (the clip's own root motion, measured from its hips), as a velocity
//      in the character's own frame (forward, left). The host rotates it by the character's heading and hands it to the capsule as `CapsuleInput::drive`.
//   2. The capsule steps. It collides, slides and steps up as it always does; it may get less far than commanded (a wall, a slope, a surface that grips less).
//   3. `advance(dt, progress)` moves the clip on by dt times the *fraction of the commanded travel the physics allowed* (`progress`, 0..1; `capsule_glue.hpp` measures it). A roll against a wall does
//      not play on at full speed with the body stuck: it slows and holds, and when it makes no headway for `stall_seconds` the action gives up and fades out.
//
// The pose is always the clip *in place* (the hips' horizontal travel removed, exactly, at every time, not on average like a loop's drift), so the character's position is the physics' alone and
// the feet that are on the ground during the action are as far from the clip's own travel as the capsule's travel is from the clip's: consistent by construction when nothing blocks it, and measured
// when something does (see the tests). `weight()` is the action's share for the host to blend over the locomotion pose (`blend_into`): it fades in over `fade_in` and out over `fade_out` at the end
// of the clip or after `cancel()`.
//
// Not modelled: the heading change of a clip (a roll that turns; the host owns the heading and the clip is assumed to keep it), the vertical travel of the hips (kept in the pose, never commanded),
// and an action that leaves the ground (the host cancels it when the capsule is no longer grounded).
#pragma once

#include "saige/animation/clip_analysis.hpp"

#include <vector>

namespace saige::anim {

struct PlanarVelocity {
    float forward = 0.0f;  // m/s along the character's facing
    float left = 0.0f;     // m/s to the character's left
    float up = 0.0f;       // m/s upward; only for an action with vertical root motion (a climb), 0 otherwise
};

struct ActionConfig {
    float fade_in = 0.08f;           // seconds
    float fade_out = 0.15f;          // seconds, before the end of the clip (or after a cancel)
    float stall_progress = 0.10f;    // a step with less progress than this counts as stalled
    float stall_seconds = 0.40f;     // this long without headway ends the action
};

// A root-motion clip prepared for playing: the hips' horizontal path through the clip, in the character's frame, sampled evenly. Immutable and shareable. Holds a pointer to the skin's skeleton
// through the clip's binding, so the skin must outlive it.
class ActionClip {
public:
    // `sample_rate` samples per second of the hips' path (linearly interpolated between samples; 60 resolves a roll or a lunge to well under a millimeter of path error). With `vertical` the hips' rise is
    // root motion too (a climb or a vault: the capsule is carried up and the pose has the rise removed, so the capsule's height is the only height); without it the vertical motion stays in the pose
    // (a roll's dip), as before. A vertical action is driven as a scripted 3D move (`CapsuleDrive::vertical`).
    ActionClip(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, const gfx::AnimationClip& clip, float sample_rate = 60.0f, bool vertical = false);

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] float duration() const noexcept { return clip_.duration; }
    // How far the hips travel over the whole clip, in the character's frame (forward, left), meters.
    [[nodiscard]] PlanarVelocity travel() const noexcept { return total_; }   // meters, not meters per second (`up` is 0 unless the clip is vertical)
    [[nodiscard]] float travel_length() const noexcept;                       // meters, including the rise of a vertical clip
    [[nodiscard]] bool vertical() const noexcept { return vertical_; }
    // The hips' offset from where they were at time 0, in the character's frame, meters; time clamped to [0, duration].
    [[nodiscard]] PlanarVelocity offset(float time) const;
    [[nodiscard]] const gfx::AnimationClip& clip() const noexcept { return clip_; }
    [[nodiscard]] const gfx::ClipBinding& binding() const noexcept { return binding_; }
    [[nodiscard]] const gfx::Skin& skin() const noexcept { return *skin_; }
    [[nodiscard]] const RigInfo& rig() const noexcept { return rig_; }

private:
    const gfx::Skin* skin_;
    RigInfo rig_;
    gfx::AnimationClip clip_;
    gfx::ClipBinding binding_;
    std::vector<PlanarVelocity> path_;  // offsets at i * step_
    float step_ = 0.0f;
    PlanarVelocity total_{};
    bool valid_ = false;
    bool vertical_ = false;
};

class ActionPlayer {
public:
    explicit ActionPlayer(ActionConfig config = {}) : cfg_(config) {}

    // Starts the action from its first frame (the clip must outlive the player's use of it). Replaces any action in progress.
    void start(const ActionClip& clip);
    // Begins fading out now; no further travel is commanded.
    void cancel();

    [[nodiscard]] bool active() const noexcept { return clip_ != nullptr && !done_; }
    [[nodiscard]] bool travelling() const noexcept { return active() && !cancelled_ && time_ < clip_->duration(); }
    [[nodiscard]] bool vertical() const noexcept { return clip_ != nullptr && clip_->vertical(); }  // the action drives the capsule as a scripted 3D move
    // The velocity to drive the character at for the next physics step of `dt` seconds (zero when not travelling): the clip's own travel over [time, time + dt].
    [[nodiscard]] PlanarVelocity command(float dt) const;
    // Moves the clip on by dt * progress (progress in [0, 1], clamped; non-finite counts as 0) and re-evaluates the pose. Throws std::invalid_argument for a non-finite or non-positive dt.
    void advance(float dt, float progress);

    [[nodiscard]] float weight() const noexcept { return weight_; }
    [[nodiscard]] float time() const noexcept { return time_; }
    [[nodiscard]] bool stalled() const noexcept { return stalled_; }   // the action ended because the physics would not let it travel
    [[nodiscard]] const gfx::Pose& pose() const noexcept { return pose_; }

private:
    void evaluate();

    ActionConfig cfg_;
    const ActionClip* clip_ = nullptr;
    float time_ = 0.0f;
    float since_start_ = 0.0f;
    float since_cancel_ = 0.0f;
    float stall_time_ = 0.0f;
    float weight_ = 0.0f;
    bool cancelled_ = false;
    bool stalled_ = false;
    bool done_ = true;
    gfx::Pose rest_;  // the pose a tick starts from: channels the clip does not drive (and the shift of the top joints) must not carry over from the last tick
    gfx::Pose pose_;
};

} // namespace saige::anim
