#pragma once

// A deterministic, fixed-step, capsule-based character controller for the first playable course (issue #62).
//
// It is independent of CharacterMotor (#65): it takes the same kind of static boxes (as Aabb) and the same fixed step, and is built
// only on the exact sweeps in capsule_sweep.hpp. The same inputs in the same build give bit-identical states, tick for tick: no
// clock, no randomness, no allocation after construction (except replace_static_geometry), and no dependence on the render rate.
//
// Z is up; +X is "forward" and +Y is "left" in the input's local sense, as in CharacterMotor.
//
// Per tick: contact recovery, input acceleration, jump on the rising edge of the button while grounded, gravity while airborne,
// collide-and-slide (up to four contacts per tick), step-up over ledges no higher than `step_height`, and a ground
// probe that keeps the character on the floor over small drops. A contact is ground only if its normal is within
// `max_slope_radians` of +Z; steeper contacts (walls, and the rim of a box the capsule only partly overhangs) are slid along, so a
// character standing on a rim slides off once the overhang is large enough.
//
// What boxes cannot express is not covered here: ramps need a non-box collider. The slope logic acts on contact normals, so it
// carries over unchanged when one exists.

#include "saige/capsule_sweep.hpp"

#include <chrono>
#include <span>
#include <vector>

namespace saige {

struct CapsuleCharacterConfig {
    float radius = 0.30f;
    float height = 1.70f;               // total, including both caps; at least 2 * radius
    float walk_speed = 3.5f;            // meters per second
    float sprint_speed = 6.0f;
    float ground_acceleration = 22.0f;  // meters per second squared toward the target speed
    float air_acceleration = 7.0f;
    float gravity = 9.81f;
    float jump_speed = 4.4f;            // launch speed; the apex is jump_speed^2 / (2 * gravity)
    float terminal_speed = 30.0f;
    float max_slope_radians = 0.87266463f;  // 50 degrees
    float step_height = 0.35f;          // the tallest ledge walked up without jumping
    float ground_snap = 0.12f;          // how far below the feet the ground is still "underfoot" (small drops, step edges)
};

struct CapsuleInput {
    float forward{};
    float left{};
    bool jump{};
    bool sprint{};
};

struct CapsuleCharacterState {
    Vec3 feet;                       // the lowest point of the capsule
    Vec3 velocity;
    bool grounded{};
    Vec3 ground_normal{0, 0, 1};     // meaningful while grounded
    bool operator==(const CapsuleCharacterState&) const = default;
};

class CapsuleCharacter {
public:
    // Throws std::invalid_argument for an invalid config, a non-finite spawn, invalid boxes, or a spawn that overlaps geometry.
    CapsuleCharacter(Vec3 spawn, std::span<const Aabb> geometry, CapsuleCharacterConfig config = {});

    // One fixed step. Throws std::invalid_argument for non-finite input or a step that is not in (0, 100 ms].
    void step(CapsuleInput input, std::chrono::nanoseconds dt);

    // Replaces the static collision between ticks. Returns false, and changes nothing, if the new set overlaps the character or the
    // reset position. Invalid boxes throw. Grounded state is recomputed against the new geometry.
    bool replace_static_geometry(std::span<const Aabb> geometry);

    // Back to the spawn, at rest.
    void reset();

    // Places the character at rest at `feet`, for example where it leaves a vehicle. A position that overlaps geometry is pushed
    // out the shortest way (several boxes are resolved in passes). Throws std::invalid_argument for a non-finite position.
    void teleport(Vec3 feet);

    [[nodiscard]] const CapsuleCharacterState& state() const noexcept { return state_; }
    [[nodiscard]] const CapsuleCharacterConfig& config() const noexcept { return config_; }
    [[nodiscard]] Capsule capsule() const noexcept { return capsule_at(state_.feet); }

private:
    [[nodiscard]] Capsule capsule_at(Vec3 feet) const noexcept { return {feet, config_.radius, config_.height}; }
    [[nodiscard]] std::optional<CapsuleHit> sweep(Vec3 feet, Vec3 displacement) const;
    [[nodiscard]] bool overlaps(Vec3 feet, std::span<const Aabb> geometry) const;
    void recover_from_overlap();
    void slide(Vec3 displacement, bool can_step);
    bool try_step_up(Vec3 leftover);
    void snap_to_ground();
    [[nodiscard]] bool ground_underfoot(Vec3 feet) const;

    CapsuleCharacterConfig config_;
    Vec3 spawn_;
    std::vector<Aabb> geometry_;
    CapsuleCharacterState state_;
    bool previous_jump_{};
};

} // namespace saige
