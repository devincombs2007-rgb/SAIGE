#pragma once

// A humanoid ragdoll on the dynamics interface (CHAR-2). Thirteen bodies (pelvis, torso, head, upper and lower arms, thighs, shins, feet) joined by constraints with anatomical limits: hinges for knees and elbows
// (flexion one way only), swing-twist cones for the spine, neck, shoulders, hips and ankles. Bodies of one ragdoll never collide with each other (limb against limb contact is left out on purpose: the joints and
// limits keep the pose plausible and self-contact is the usual source of jitter); they collide with everything else.
//
// Three things around it: animation to ragdoll (`start` takes the model-space pose now and a moment ago, so the ragdoll inherits the animation's velocity: momentum is continuous at the switch), ragdoll to animation
// (`RagdollRecovery` blends from the physical pose to the animated one with a smooth weight, and `lying_face_up` says which get-up to pick), and a first form of motor-driven physical animation (`drive_to_pose`: joint
// motors toward an animated pose, and `balance`: an ankle strategy that keeps the center of mass over the feet). It is independent of the animation module: a pose is
// thirteen world transforms, and mapping a skeleton's joints onto the bones is the caller's (the animation glue).
//
// Frame: SAIGE axes (+X forward, +Y left, +Z up) in the spec; the world's own frame is used for every position.
#include "saige/dynamics.hpp"

#include <array>
#include <cstddef>
#include <vector>

namespace saige {

enum class Bone : std::uint8_t { Pelvis, Torso, Head, UpperArmL, UpperArmR, ForearmL, ForearmR, ThighL, ThighR, ShinL, ShinR, FootL, FootR, Count };
inline constexpr std::size_t kBoneCount = static_cast<std::size_t>(Bone::Count);

struct BoneTransform {
    DVec position;
    DQuat rotation;
};
using RagdollPose = std::array<BoneTransform, kBoneCount>;

// The measurements of a body in its rest pose (a standing T-less pose, arms hanging), world space. `standard_humanoid` fills them from a height.
struct HumanoidSpec {
    DVec forward{1, 0, 0};
    DVec up{0, 0, 1};
    double mass = 75.0;
    double height = 1.75;
    DVec pelvis, chest, neck, head;                       // body centers or joints along the spine
    DVec shoulder[2], elbow[2], wrist[2];                 // [0] left, [1] right
    DVec hip[2], knee[2], ankle[2], toe[2];
    double radius_arm = 0.04, radius_forearm = 0.035, radius_thigh = 0.075, radius_shin = 0.055, radius_head = 0.11;
};

// Anatomical proportions (Winter, de Leva) for a person `height` tall standing with their feet centered at `feet`, facing `forward` (horizontal), arms hanging.
[[nodiscard]] HumanoidSpec standard_humanoid(double height, double mass, DVec feet, DVec forward = {1, 0, 0});

struct RagdollLimits {
    double spine_swing = 0.35, spine_twist = 0.3;
    double neck_swing = 0.6, neck_twist = 0.7;
    double shoulder_swing = 1.7, shoulder_twist = 0.9;
    double elbow_flexion = 2.5;     // radians from straight
    double hip_swing = 1.3, hip_twist = 0.5;
    double knee_flexion = 2.4;
    double ankle_swing = 0.55, ankle_twist = 0.25;
};

class Ragdoll {
public:
    // Builds the bodies in the rest pose of `spec`, at rest. `group` is the self-collision group (nonzero, unique per ragdoll alive in the world). Throws std::invalid_argument for a bad spec or group, and whatever the world throws.
    Ragdoll(DynamicsWorld& world, const HumanoidSpec& spec, std::uint32_t group, const RagdollLimits& limits = {});
    Ragdoll(const Ragdoll&) = delete;
    Ragdoll& operator=(const Ragdoll&) = delete;
    Ragdoll(Ragdoll&&) noexcept;
    Ragdoll& operator=(Ragdoll&&) noexcept;
    ~Ragdoll();

    // The rest pose of the spec (world transforms of the thirteen bodies), the pose an animation maps onto.
    [[nodiscard]] const RagdollPose& rest_pose() const noexcept { return rest_; }
    // Animation to ragdoll: places every body at `now` and gives it the linear and angular velocity that took `before` to `now` in `dt` seconds. A momentary pose with `before == now` starts at rest.
    void start(const RagdollPose& now, const RagdollPose& before, double dt);
    [[nodiscard]] RagdollPose pose() const;
    [[nodiscard]] BodyId body(Bone bone) const noexcept { return bodies_[static_cast<std::size_t>(bone)]; }
    [[nodiscard]] double total_mass() const noexcept { return spec_.mass; }
    [[nodiscard]] DVec center_of_mass() const;
    [[nodiscard]] DVec linear_momentum() const;
    // The character's front (body-local +X of the torso, rotated) is toward the sky: lying on the back. The get-up animation to pick.
    [[nodiscard]] bool lying_face_up() const;

    // Motor-driven physical animation: every joint gets a spring-damper motor (the world solves it with the constraints, so it is stable at any stiffness and while the feet carry the weight) toward the joint orientations of
    // `target` (child relative to parent). `motor.frequency` is the joint's own natural frequency in Hz (4 Hz is a loose follower, 8 to 12 Hz a stiff one), `damping` 1 is critical, `max_torque` caps each joint (0: no cap).
    // The pelvis has no joint above it: its orientation is whatever the legs and the ground make of it. Call once, or again each time the target pose changes (an animation drives it by calling it every frame).
    // The hips, knees and ankles carry the body: a joint motor's stiffness follows the reduced mass of the two bodies it joins (a foot against a shin), not the weight above, so the load-bearing joints run at `support_scale`
    // times the frequency (their springs must be stiff enough to hold the body up: 735 N m per rad at the ankle for 75 kg and a 1 m lever).
    void drive_to_pose(const RagdollPose& target, const DMotor& motor, double support_scale = 1.0);
    // Back to limp: every motor off.
    void release();
    // Ankle strategy on top of drive_to_pose: the feet's targets are tilted by `gain` (rad per m) times the offset of the center of mass from the middle of the feet, plus `damping` (rad per m/s) times its velocity,
    // forward and sideways, limited to +-0.35 rad, which moves the center of pressure under the center of mass. Call every step after drive_to_pose (it re-targets the ankle motors).
    void balance(const RagdollPose& target, const DMotor& motor, double gain, double damping, double support_scale = 1.0);

private:
    DynamicsWorld* world_ = nullptr;
    HumanoidSpec spec_;
    RagdollPose rest_;
    std::array<BodyId, kBoneCount> bodies_;
    std::array<double, kBoneCount> mass_{};
    struct Joint {
        ConstraintId id;
        Bone parent, child;
    };
    std::vector<Joint> joints_;
};

// Ragdoll to animation: a weight that rises from 0 (physical) to 1 (animated) over `duration` seconds with a smooth ease, and the blend of two poses by it (positions linearly, rotations by the shortest-arc slerp).
class RagdollRecovery {
public:
    explicit RagdollRecovery(double duration);
    void restart() noexcept { time_ = 0.0; }
    void advance(double dt) noexcept;
    [[nodiscard]] double weight() const noexcept;     // 0 at the start, 1 once `duration` has passed; smoothstep in between
    [[nodiscard]] bool finished() const noexcept { return time_ >= duration_; }
    [[nodiscard]] RagdollPose blend(const RagdollPose& physical, const RagdollPose& animated) const;

private:
    double duration_, time_ = 0.0;
};

} // namespace saige
