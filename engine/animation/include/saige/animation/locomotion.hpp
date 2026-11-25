// SAIGE animation: a reusable locomotion controller for a humanoid character: idle, walk, jog, run and sprint blended by speed with stride-matched playback, jumping, falling and landing.
//
// Ownership (see docs/animation-character.md). The physics owns the character: its position, velocity and collision are the CapsuleCharacter's (or any controller's), and the animation never moves
// it. The animation owns only the pose. Locomotion clips are played *in place* (any travel a clip carries is removed, see clip_analysis.hpp) and the controller is driven by the speed the character
// really has this tick, not by the input: a character pushing into a wall has speed 0 and so is not running on the spot, and a character on a slope or a surface that slows it plays slower.
//
// Stride matching. Each locomotion cycle has a measured `ground_speed` and `stride_length` (the distance one full cycle covers). At speed v the cycles on either side of v are blended, and the
// cycle rate is v divided by the blended stride, so the feet that are planted in the blend move backward at exactly v: they do not slide. All cycles share one phase, aligned so that the left foot
// lands at the same phase in every cycle, so the legs of two gaits are in step while they blend.
//
// What the controller keeps and what it does not: one-shot animations (jump start, land) fade in and out over short crossfades; a second jump while airborne is not modelled; strafing and turning
// in place are not yet (the character is assumed to move along its facing, which the host sets on the model transform).
#pragma once

#include "saige/animation/clip_analysis.hpp"

#include <memory>
#include <string>
#include <vector>

namespace saige::anim {

struct LocomotionCycle {
    gfx::AnimationClip clip;
    gfx::ClipBinding binding;
    ClipInfo info;
};

struct SetClip {
    gfx::AnimationClip clip;
    gfx::ClipBinding binding;
    bool present = false;
};

// The animation data of one character: a skeleton, its measured cycles (ascending ground speed) and the other clips. Built once, shared by every controller of that skeleton. It holds a pointer to the
// skin, which must outlive it.
class LocomotionSet {
public:
    LocomotionSet(const gfx::Skin& skin, const HumanoidMap& map);

    // Adds a locomotion cycle (walk, jog, run, sprint). Returns false and says why in `why` if the clip is not a clean locomotion cycle (no planted feet, a poor loop, or no measurable speed).
    // The cycles are kept sorted by ground speed; two cycles within 0.15 m/s of each other keep the better loop.
    bool add_cycle(const gfx::AnimationClip& clip, std::string* why = nullptr, float max_loop_error = 0.35f);
    void set_idle(const gfx::AnimationClip& clip);        // a looping standing clip
    void set_jump_start(const gfx::AnimationClip& clip);  // one-shot: leaving the ground
    void set_air(const gfx::AnimationClip& clip);         // looping: rising and falling
    void set_land(const gfx::AnimationClip& clip);        // one-shot: touching down

    [[nodiscard]] const gfx::Skin& skin() const noexcept { return *skin_; }
    [[nodiscard]] const HumanoidMap& map() const noexcept { return map_; }
    [[nodiscard]] const RigInfo& rig() const noexcept { return rig_; }
    [[nodiscard]] const std::vector<LocomotionCycle>& cycles() const noexcept { return cycles_; }
    [[nodiscard]] const SetClip& idle() const noexcept { return idle_; }
    [[nodiscard]] const SetClip& jump_start() const noexcept { return jump_start_; }
    [[nodiscard]] const SetClip& air() const noexcept { return air_; }
    [[nodiscard]] const SetClip& land() const noexcept { return land_; }
    [[nodiscard]] bool usable() const noexcept { return !cycles_.empty(); }
    // Phase at which the left foot lands in every cycle once aligned (the first cycle's own).
    [[nodiscard]] float reference_phase() const noexcept { return cycles_.empty() ? 0.0f : cycles_.front().info.left.contact_start; }

private:
    const gfx::Skin* skin_;
    HumanoidMap map_;
    RigInfo rig_;
    std::vector<LocomotionCycle> cycles_;
    SetClip idle_, jump_start_, air_, land_;
};

struct LocomotionConfig {
    float jump_vertical_threshold = 1.2f;  // m/s of upward speed that makes leaving the ground a jump (below it, a fall)
    float fade_ground = 0.15f;             // seconds of crossfade into the ground state
    float fade_air = 0.10f;                // into the jump or the air
    float fade_land = 0.08f;               // into a landing
    float jump_start_rate = 2.0f;          // the takeoff clip is played this much faster than authored (a real jump leaves the ground sooner than a keyed one)
    float land_rate = 1.6f;
    float land_cut_speed = 1.5f;           // m/s: landing while moving faster than this blends back to running early
    float max_rate_scale = 1.6f;           // the fastest cycle is played at most this much faster than authored; beyond it the feet slide
    float speed_smoothing = 0.12f;         // seconds: the speed that picks the blend is smoothed, so that a sudden change of speed fades poses over a few tenths of a second instead of popping
    float idle_on = 0.12f;                 // fraction of the slowest cycle's speed above which the character leaves the idle (the slowest cycle then plays slower, matched to the speed)
    float idle_off = 0.06f;                // fraction below which it returns to the idle (the gap is a hysteresis, so a speed hovering at the threshold does not flicker)
    float fade_idle = 0.20f;               // seconds the crossfade between the idle and the cycles takes, however suddenly the speed changes
    bool stride_matching = true;           // false plays every cycle at its authored rate whatever the speed (feet then slide; kept for comparison in tests)
};

struct LocomotionInput {
    float speed = 0.0f;           // horizontal speed the character really has, m/s (>= 0)
    bool grounded = true;
    float vertical_speed = 0.0f;  // m/s, positive up
};

enum class LocomotionState : std::uint8_t { Ground, JumpStart, Air, Land };

class LocomotionController {
public:
    explicit LocomotionController(const LocomotionSet& set, LocomotionConfig config = {});

    // Advances by dt seconds (finite, positive) and updates the pose. Throws std::invalid_argument for a non-finite or non-positive dt or a non-finite input; non-negative speed is clamped.
    void update(float dt, const LocomotionInput& input);

    [[nodiscard]] const gfx::Pose& pose() const noexcept { return pose_; }
    [[nodiscard]] LocomotionState state() const noexcept { return state_; }
    [[nodiscard]] float phase() const noexcept { return phase_; }                 // cycle phase in [0, 1)
    [[nodiscard]] float cycle_rate() const noexcept { return rate_; }             // cycles per second of the ground blend, last update
    [[nodiscard]] float stride_speed() const noexcept { return stride_speed_; }   // the speed the planted feet of the ground blend move at, last update (equals the input speed within the clip range)
    [[nodiscard]] const std::vector<float>& cycle_weights() const noexcept { return weights_; }  // per cycle of the set, last update
    [[nodiscard]] float idle_weight() const noexcept { return idle_weight_; }

private:
    struct Layer {
        LocomotionState state;
        float time;    // seconds in a one-shot or the air loop
        float weight;
        float fade;    // seconds this layer takes to reach weight 1
    };
    void enter(LocomotionState s, float fade);
    void evaluate_ground(gfx::Pose& out);
    void evaluate_layer(const Layer& layer, gfx::Pose& out);

    const LocomotionSet* set_;
    LocomotionConfig cfg_;
    LocomotionState state_ = LocomotionState::Ground;
    std::vector<Layer> layers_;
    gfx::Pose pose_;
    float phase_ = 0.0f;
    float idle_time_ = 0.0f;
    float speed_ = 0.0f;       // smoothed
    float rate_ = 0.0f;
    float stride_speed_ = 0.0f;
    float idle_weight_ = 1.0f;
    float loco_ = 0.0f;           // 0 idle .. 1 cycles, slewed over fade_idle
    bool moving_ = false;
    std::vector<float> weights_;
    float state_time_ = 0.0f;
};

} // namespace saige::anim
