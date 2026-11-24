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
// The ground probe slides down along a steep contact it already touches (the top edge of a low ledge the character is sliding along)
// instead of stopping at it, so such a contact does not hide the floor beneath: grounded does not flicker and the feet do not bounce.
//
// Supported range. Positions are chunk-local meters (capsule_sweep.hpp): the capsule must stay within +-max_local_extent on every axis, and
// the configuration must keep every sweep within max_sweep_length (checked at construction). The constructor and teleport throw for a
// position outside the range; a step that would carry the capsule across it throws and changes nothing. The host rebases larger world
// coordinates and streams chunks; the controller never answers from a lower-precision sweep. Contacts are applied from
// CapsuleHit::stop (the sweep's float position clear of the geometry), never from `feet + displacement * t`.
//
// What boxes cannot express is not covered here: ramps need a non-box collider. The slope logic acts on contact normals, so it
// carries over unchanged when one exists.

#include "saige/capsule_sweep.hpp"
#include "saige/capsule_triangle.hpp"

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

// A velocity the host drives the character at while it stands on the ground, in the same world axes as the state's velocity (+X, +Y horizontal, meters per second). An action that carries the character (a
// roll, a lunge: the root motion of an animation clip) uses it: the horizontal velocity is set to the drive instead of accelerating toward the input's target, and the character still collides, slides,
// steps up and keeps to the ground exactly as it does when walking, so an action can never carry it through geometry. In the air it is ignored (the air keeps its own rules); a non-finite drive is an
// invalid input and its magnitude is limited to the terminal speed. Inactive (the default) changes nothing: the step is bit-identical to one with no drive field.
struct CapsuleDrive {
    bool active{};
    float x{};
    float y{};
};

struct CapsuleInput {
    float forward{};
    float left{};
    bool jump{};
    bool sprint{};
    // How well the feet grip what the character stands on, in (0, 1]: 1 (the default) is the configured ground acceleration; less is a slipperier surface (ice) on which the character takes longer to get
    // up to speed and longer to stop. The host derives it from the surface under the feet (see physics_material.hpp: `ground_traction`); the controller does not know about materials. Clamped to
    // [0.02, 1]; it only affects the ground, never the air. A non-finite value is an invalid input.
    float traction = 1.0f;
    CapsuleDrive drive{};
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
    // Throws std::invalid_argument for an invalid config (including one whose reach exceeds the sweep envelope), a non-finite or out-of-range
    // spawn, invalid boxes, or a spawn that overlaps geometry.
    CapsuleCharacter(Vec3 spawn, std::span<const Aabb> geometry, CapsuleCharacterConfig config = {});
    // The same with static triangles (ramps and other non-box surfaces, see capsule_triangle.hpp) alongside the boxes. Invalid triangles
    // throw. Triangles are two-sided; the slope, step and ground rules below apply to their contact normals unchanged. The configuration
    // is required (no default): with one, `CapsuleCharacter({}, {}, {})` would be ambiguous between this and the box constructor taking
    // a configuration, which existing code writes. Pass `{}` or `CapsuleCharacterConfig{}` for the defaults.
    CapsuleCharacter(Vec3 spawn, std::span<const Aabb> boxes, std::span<const Triangle> triangles, CapsuleCharacterConfig config);

    // One fixed step. Throws std::invalid_argument for non-finite input or a step that is not in (0, 100 ms], and for a character whose
    // capsule has reached the edge of the supported envelope (capsule_sweep.hpp). Strong guarantee: a throwing step changes nothing.
    void step(CapsuleInput input, std::chrono::nanoseconds dt);

    // Replaces the static collision between ticks. Returns false, and changes nothing, if the new set overlaps the character or the
    // reset position. Invalid boxes throw. Grounded state is recomputed against the new geometry. Replacing with boxes alone is a
    // complete-set replacement: it clears the triangles (and the two-argument form replaces both).
    bool replace_static_geometry(std::span<const Aabb> geometry);
    // The same for boxes and triangles together.
    bool replace_static_geometry(std::span<const Aabb> boxes, std::span<const Triangle> triangles);
    // Translates the live and reset poses into a new local frame; geometry is already in that frame.
    // Preserves input/velocity history. Rejected geometry or an exception leaves this controller unchanged.
    bool migrate_origin(Vec3 shift, std::span<const Aabb> boxes, std::span<const Triangle> triangles);

    // Save the current grounded pose as the reset position.
    bool remember_grounded_checkpoint() noexcept;

    // Back to the spawn, at rest.
    void reset();

    // Places the character at rest at `feet`, for example where it leaves a vehicle. A position that overlaps geometry is pushed
    // out the shortest way (several boxes are resolved in passes). Returns false, and changes nothing, if bounded recovery cannot
    // leave the capsule clear (for example a gap narrower than the capsule between two tall boxes), so a blocked exit keeps the rider's
    // previous state. Throws std::invalid_argument for a non-finite position or one outside the supported range, changing nothing.
    [[nodiscard]] bool teleport(Vec3 feet);

    [[nodiscard]] const CapsuleCharacterState& state() const noexcept { return state_; }
    [[nodiscard]] const CapsuleCharacterConfig& config() const noexcept { return config_; }
    [[nodiscard]] Capsule capsule() const noexcept { return capsule_at(state_.feet); }

private:
    [[nodiscard]] Capsule capsule_at(Vec3 feet) const noexcept { return {feet, config_.radius, config_.height}; }
    [[nodiscard]] std::optional<CapsuleHit> sweep(Vec3 feet, Vec3 displacement) const;
    [[nodiscard]] bool overlaps(Vec3 feet, std::span<const Aabb> boxes, std::span<const Triangle> triangles) const;
    bool recover_from_overlap();  // false (and the feet restored to where they were) if the capsule is still overlapping after the bounded passes
    void step_unchecked(CapsuleInput input, std::chrono::nanoseconds dt);
    void slide(Vec3 displacement, bool can_step);
    bool try_step_up(Vec3 leftover);
    void snap_to_ground();
    struct GroundContact {
        Vec3 feet;      // where the capsule stands once lowered onto the ground
        Vec3 normal;
    };
    [[nodiscard]] std::optional<GroundContact> probe_ground(Vec3 feet) const;
    [[nodiscard]] bool ground_underfoot(Vec3 feet) const;

    CapsuleCharacterConfig config_;
    Vec3 spawn_;
    std::vector<Aabb> geometry_;
    std::vector<Triangle> triangles_;
    CapsuleCharacterState state_;
    bool previous_jump_{};
    bool steep_contact_{};  // the last tick met a surface that faces up but is too steep to stand on (a slide, not a fall)
};

} // namespace saige
