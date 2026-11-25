// SAIGE animation: getting into a vehicle. A reusable foundation, not a showcase's choreography: a state machine that takes a character from walking up to a car, through reaching the door handle, opening
// the door, stepping in and sitting down, and produces per tick what the rest needs: the character's root, the door's angle and handle position (the handle follows the hinge), the weights and targets for the hands
// (door handle, then the steering wheel) and the feet (the pedals), and how far the hips are lowered. `apply_entry_pose` turns that frame into a pose with the engine's IK (arm constraints for the hands, two-bone IK for the legs).
//
// Everything about the vehicle is data in the vehicle's local frame (+X forward, +Y left, +Z up, the physics Vehicle's frame): where the hinge, the handle, the opening, the seat, the wheel and the pedals are.
// What this does not do: no stepping (the feet slide into the footwell while the hips lower), no door collision (the host disables the character's collision with the vehicle during the entry and the door is
// not a physical body), no closing the door, no exiting (the reverse is a different state machine), and nothing here moves the vehicle: `begin` refuses a vehicle that is moving.
#pragma once

#include "saige/animation/humanoid.hpp"
#include "saige/animation/ik.hpp"

namespace saige::anim {

struct VehicleEntryRig {
    render::Float3 hinge_point{0.9f, 0.9f, 0.6f};     // a point on the door's hinge axis
    render::Float3 hinge_axis{0.0f, 0.0f, 1.0f};      // unit; a positive angle swings the door open (the handle away from the body)
    render::Float3 handle{0.0f, 1.0f, 0.95f};         // the handle with the door closed
    float door_max = 1.1f;                            // radians
    bool door_hand_left = true;                       // the hand that takes the handle
    render::Float3 stand_point{0.0f, 1.7f, 0.0f};     // on the ground beside the door, where the reach begins
    float stand_yaw = 0.0f;                           // the character's heading there, relative to the vehicle's (0: facing along it)
    render::Float3 open_step_back{0.0f, 0.15f, 0.0f}; // how far the character moves while the door swings, to keep the handle in reach
    render::Float3 threshold{0.3f, 0.9f, 0.0f};       // on the ground in the door opening, which the character passes through
    render::Float3 seat_root{-0.2f, 0.45f, 0.0f};     // the floor under the hips when seated
    float seat_hips_height = 0.5f;                    // hips above that floor when seated, m
    float seat_yaw = 0.0f;                            // relative to the vehicle's heading
    render::Float3 wheel_left{0.35f, 0.15f, 0.95f};   // where each hand grips the steering wheel
    render::Float3 wheel_right{0.35f, -0.15f, 0.95f};
    render::Float3 pedal_left{0.7f, 0.1f, 0.12f};     // where each foot's contact point rests seated
    render::Float3 pedal_right{0.7f, -0.1f, 0.12f};
};

struct VehicleEntryConfig {
    float walk_speed = 1.4f;       // m/s of the approach
    float turn_rate = 5.0f;        // rad/s
    float reach_time = 0.5f;       // s
    float open_time = 0.9f;
    float enter_time = 1.4f;
    float settle_time = 0.4f;
    float max_vehicle_speed = 0.5f;  // m/s: begin refuses a vehicle moving faster
};

enum class EntryPhase : std::uint8_t { Approach, Reach, Open, Enter, Seated };

struct EntryFrame {
    EntryPhase phase = EntryPhase::Approach;
    float phase_time = 0.0f;          // seconds in the phase
    render::Transform root;           // the character's root in the world (heading about +Z, scale 1)
    float door_angle = 0.0f;
    render::Float3 door_handle;       // world: where the handle is now
    float door_hand_weight = 0.0f;    // 0..1
    render::Float3 wheel_left, wheel_right;  // world grip points
    float wheel_weight = 0.0f;        // 0..1 (both hands)
    float hips_drop = 0.0f;           // m below the standing hips
    float seat_weight = 0.0f;         // 0..1: the legs go from planted to the pedals
    render::Float3 pedal_left, pedal_right;  // world foot contact targets
    bool finished = false;            // seated and settled
};

class VehicleEntry {
public:
    // Throws std::invalid_argument for non-finite or non-positive times, a zero hinge axis or a negative door range.
    VehicleEntry(const VehicleEntryRig& rig, const VehicleEntryConfig& config = {});
    // Starts the entry from where the character stands (world root position and heading, standing hips `standing_hips` above the root). False (nothing started) when the vehicle is faster than
    // `max_vehicle_speed` or an input is not finite.
    bool begin(const render::Transform& vehicle, render::Float3 root_position, float root_yaw, float standing_hips, float vehicle_speed);
    // Advances by dt (positive, finite) with the vehicle's current world transform (a parked vehicle can still settle on its springs).
    const EntryFrame& step(float dt, const render::Transform& vehicle);
    [[nodiscard]] const EntryFrame& frame() const noexcept { return frame_; }
    [[nodiscard]] bool active() const noexcept { return started_; }

private:
    VehicleEntryRig rig_;
    VehicleEntryConfig config_;
    EntryFrame frame_;
    bool started_ = false;
    float standing_hips_ = 0.9f;
    render::Float3 enter_from_;       // vehicle-local root at the start of Enter
    float enter_yaw_from_ = 0.0f;     // world heading at the start of Enter, relative to the vehicle's
};

// Poses the character for an entry frame: the hips lowered by `hips_drop`, the feet kept where the animation had them (knees bending) and moved to the pedals by `seat_weight`, the door hand on the handle and
// the hands on the wheel by their weights (through reach_hand with `arms`, so the elbow and shoulder stay in their range and out of the body; null for none). `root` is `frame.root`. Returns the largest
// distance, in meters, a hand at full weight is from its wanted target, measured on the pose (0 when none is active); a target inside the body volumes is a miss.
float apply_entry_pose(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, gfx::Pose& pose, const EntryFrame& frame, bool door_hand_left, const ArmConstraints* arms);

} // namespace saige::anim
