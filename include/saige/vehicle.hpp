#pragma once

// A rigid-body, four-wheel raycast vehicle on a StaticCollision snapshot (issue #43, Showcase 03; reusable engine functionality, not a showcase trick).
//
// The chassis is one rigid body with six degrees of freedom: a position, an orientation, a linear and an angular velocity, integrated at a fixed
// substep (1/120 s) with semi-implicit Euler, in double. Each wheel is a sphere probe (wheel_contact.hpp) from the top of its suspension travel along the
// chassis' down axis. A contact compresses a spring-damper that pushes the chassis along its up axis; the tire then pushes in the contact plane: a drive
// force (engine force with a power limit), a brake force, and a lateral force that cancels the sideslip, all inside a friction circle of radius mu * load.
// Steering turns the front wheels about the chassis' up axis, less at speed. Aerodynamic drag and rolling resistance act on the body.
//
// The chassis is an oriented box (oriented_box.hpp) tested against the collision snapshot every substep. Every penetrating primitive gets a positional
// correction and an impulse at the box's deepest vertex (a single-point manifold): normal impulse with restitution, then friction bounded by the Coulomb
// cone, so an off-centre impact produces yaw and roll. Impacts above a speed threshold are recorded as events (speed, point, normal, energy) for effects
// and sound to consume; nothing is faked from the event.
//
// What this is not: no tire deformation or tire slip-ratio state (wheel speed is the ground speed; traction is the friction circle), no suspension geometry beyond a vertical spring, no anti-roll bar, no vehicle-vehicle contact, and one contact point per primitive. Everything is deterministic for the same inputs and build.
//
// Frame: local +X forward, +Y left, +Z up (counter-clockwise from above, as make_oriented_box). Positions are chunk-local (see capsule_sweep.hpp's envelope).

#include "saige/physics_material.hpp"
#include "saige/static_collision.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace saige {

struct VehicleQuat {
    float x{}, y{}, z{}, w{1.0f};
};

struct VehicleWheelConfig {
    Vec3 mount;          // top of the suspension travel, relative to the chassis center (the wheel center when fully drooped is `travel` below it)
    bool steered{};
    bool driven{};
};

// An engine, gearbox and final drive in place of the flat `engine_force` and `engine_power` (which stay the model when `enabled` is false: the default, bit-identical to before). The engine's torque
// follows a curve over its speed (piecewise linear between the points, rpm ascending); the engine speed is the wheels' times the gear and final drive, never below `launch_rpm` while the clutch
// slips from a standstill; the drive force at the wheels is torque * gear * final drive * efficiency / wheel radius, shared by the driven wheels, and the tires' friction circle still limits what
// reaches the ground. An automatic gearbox shifts up past `upshift_rpm` and down below `downshift_rpm` (never faster than `shift_time`, during which the engine delivers no torque); reverse is
// engaged from a standstill by a negative throttle (a positive throttle while still rolling backward acts through first gear, which brakes it). Above the rev limit the engine delivers nothing. The engine speed and the gear are published in `VehicleState` (for engine sound and gauges).
struct VehiclePowertrain {
    bool enabled = false;
    std::array<std::array<float, 2>, 6> torque_curve{{{1000, 150}, {2000, 220}, {3000, 265}, {4500, 285}, {6000, 260}, {7000, 200}}};  // {rpm, N m}
    float idle_rpm = 900.0f;
    float launch_rpm = 3000.0f;
    float redline_rpm = 6800.0f;
    std::array<float, 6> gear_ratios{{3.6f, 2.2f, 1.5f, 1.1f, 0.85f, 0.7f}};
    int gear_count = 6;                // how many of the ratios are used (1 to 6)
    float reverse_ratio = 3.4f;
    float reverse_speed_limit = 7.0f;  // m/s: the engine delivers nothing in reverse past this speed
    float final_drive = 3.7f;
    float drivetrain_efficiency = 0.9f;
    float upshift_rpm = 6000.0f;
    float downshift_rpm = 2600.0f;
    float shift_time = 0.25f;          // seconds
    // Engine braking: with the throttle released (and the clutch engaged, above a walking pace) the engine's own friction and pumping losses drag the driven wheels back. This is the drag torque at the redline, N m,
    // scaled linearly with the engine speed and multiplied through the gear and final drive, so it is strongest in a low gear at speed. 0 is off (and bit-identical to before).
    float engine_brake = 0.0f;
};

// How the drive force is shared between the driven wheels. Legacy (the default) splits it equally and lets the tires' friction circles clip each share, exactly as before. The others use the friction
// limit each driven wheel had on the previous substep: Open gives every wheel the same force, so it is capped by the wheel with the least grip (a wheel on ice or in the air starves the other); Locked
// shares the force in proportion to each wheel's grip (an idealised locked axle: no wheel-speed state is modelled, so it does not bind in a corner); LimitedSlip lets a wheel take up to `lsd_bias` times what the
// weakest driven wheel can carry plus `lsd_preload`.
enum class VehicleDifferential : std::uint8_t { Legacy, Open, Locked, LimitedSlip };

// The lateral tire force over the friction limit at a slip angle (VehicleConfig::slip_angle_lateral): sin(C atan(B |a|)) with C = (2/pi)(pi - asin(slide_ratio)). It is 0 at no slip, rises with initial slope C*B per radian,
// peaks at 1 where atan(B |a|) = pi / (2 C), and falls to `slide_ratio` as the angle grows. Even in the angle (the sign of the force opposes the sliding, applied by the caller).
[[nodiscard]] double tire_lateral_ratio(double slip_angle, double stiffness, double slide_ratio) noexcept;

struct VehicleConfig {
    float mass = 1450.0f;                       // kg
    Vec3 half_extents{2.15f, 0.92f, 0.48f};     // the chassis box, meters
    Vec3 center_of_mass{0.0f, 0.0f, -0.28f};    // relative to the chassis box center
    float wheel_radius = 0.33f;
    float suspension_travel = 0.62f;            // from the mount to the wheel center at full droop (the probe length)
    float spring = 26000.0f;                    // N/m per wheel
    float damper = 3200.0f;                     // N s/m per wheel
    float tire_friction = 1.15f;                // mu, the friction circle
    float engine_force = 7800.0f;               // N at the driven wheels together, at low speed
    float engine_power = 120000.0f;             // W: caps the drive force at speed
    float brake_force = 15500.0f;               // N at all wheels together
    float max_steer = 0.52f;                    // rad at standstill; reduced with speed
    float steer_speed_scale = 16.0f;            // m/s at which the steering angle has fallen to a fraction of its maximum
    float drag_area_cd = 0.62f;                 // 0.5 * rho * Cd * A in kg/m: the drag force is this times speed squared
    float rolling_resistance = 140.0f;          // N at the wheels together, opposing the motion
    float body_restitution = 0.18f;
    float body_friction = 0.55f;
    float impact_event_speed = 1.5f;            // m/s of approach below which a collision is not reported
    // Handling options, all off by default (the defaults reproduce the vehicle exactly as before they existed).
    // Tire load sensitivity: a tire's grip coefficient falls as its load rises above the static share of the weight, mu * (1 - k * (load / static load - 1)) clamped to [0.5, 1.5], so load transferred to the
    // outside wheels in a corner does not buy back what the inside wheels lose (real tires: k of about 0.1 to 0.3). 0 is off.
    float tire_load_sensitivity = 0.0f;
    // Handbrake (VehicleInput::handbrake, 0 to 1): brake force at the non-steered wheels together, and the fraction of its grip their friction circle keeps at full handbrake (1 would be no loss; the
    // rear wheels then slide, which is what makes the tail come round).
    float handbrake_force = 9000.0f;
    float handbrake_grip = 0.45f;
    VehiclePowertrain powertrain;
    VehicleDifferential differential = VehicleDifferential::Legacy;
    float lsd_bias = 2.5f;       // LimitedSlip: the most a wheel may carry as a multiple of the weakest driven wheel's grip (1 is an open differential)
    float lsd_preload = 300.0f;  // LimitedSlip: N of extra force a wheel may take over the bias, per driven wheel
    // Lateral tire force from the slip angle instead of cancelling the sideslip outright: a Pacejka-like curve F = limit * sin(C atan(B a)) with `a` the slip angle of the contact (atan of the lateral over
    // the longitudinal speed, the latter floored at 1.5 m/s), rising with stiffness B, peaking, then falling to `lateral_slide_ratio` of the peak (a sliding tire grips less than a rolling one: this is what lets
    // a tail stay out in a drift and a rear that has broken away under power stay broken). C follows from the ratio. The force never exceeds what would cancel the sideslip in one substep, so the
    // low-speed behavior stays that of the original model. The friction circle still limits the sum with the drive and brake force. Off by default (bit-identical).
    bool slip_angle_lateral = false;
    float lateral_stiffness = 12.0f;     // B, per radian of slip angle: the peak is at atan(B a) = pi / (2 C)
    float lateral_slide_ratio = 0.85f;   // the force at a large slip angle over the peak, in [0.3, 1]
    std::array<VehicleWheelConfig, 4> wheels{{
        {{1.30f, 0.80f, -0.05f}, true, false},    // front left
        {{1.30f, -0.80f, -0.05f}, true, false},   // front right
        {{-1.30f, 0.80f, -0.05f}, false, true},   // rear left
        {{-1.30f, -0.80f, -0.05f}, false, true},  // rear right
    }};
};

// Throws std::invalid_argument for a non-finite or out-of-range value (non-positive mass, radius, spring, damper or extents, a friction or restitution
// outside its range, a wheel mount outside the chassis' extent plus the travel).
void validate_vehicle_config(const VehicleConfig& config);

struct VehicleInput {
    float throttle = 0.0f;  // -1 (reverse) to 1
    float brake = 0.0f;     // 0 to 1
    float steer = 0.0f;     // -1 (right) to 1 (left)
    float handbrake = 0.0f; // 0 to 1: brakes the non-steered wheels and takes away part of their grip (VehicleConfig::handbrake_*)
};

struct VehicleWheelState {
    bool in_contact{};
    float compression{};     // meters of spring compression (0 when drooped)
    float steer_angle{};     // rad
    float spin{};            // accumulated rotation about the axle, rad (forward motion is positive), for the wheel's visual
    Vec3 contact_point;
    Vec3 contact_normal;
    float load{};            // N pushed through the spring, last substep
    float drive_force{};     // N of longitudinal force the tire put on the chassis in the last substep (drive, engine braking, brake, rolling resistance; positive forward)
    float slip_angle{};      // rad, signed: atan of the contact's lateral over longitudinal speed (the longitudinal floored at 1.5 m/s); positive when the contact slides to the left
};

struct VehicleImpact {
    std::uint64_t substep{};
    float speed{};           // approach speed along the contact normal at the contact point, m/s
    float impulse{};         // N s of normal impulse
    float energy{};          // J lost to the collision (kinetic energy before minus after)
    Vec3 point;
    Vec3 normal;             // unit, from the primitive toward the chassis
    StaticPrimitive primitive;
};

struct VehicleState {
    Vec3 position;           // the chassis box center
    VehicleQuat orientation;
    Vec3 velocity;           // m/s of the center of mass, world
    Vec3 angular_velocity;   // rad/s, world
    std::array<VehicleWheelState, 4> wheels;
    float forward_speed{};   // along the chassis' +X
    float yaw{};             // heading of the chassis' +X in the horizontal plane
    int wheels_in_contact{};
    int gear{};              // with a powertrain: 1 to gear_count, -1 in reverse; 0 without one
    float engine_rpm{};      // with a powertrain; 0 without one
};

class Vehicle {
public:
    static constexpr float substep = 1.0f / 120.0f;

    // Places the chassis level at `position` (the box center) with the given heading, wheels drooped, at rest. Throws on an invalid config.
    Vehicle(const VehicleConfig& config, Vec3 position, float yaw_radians);

    // Advances by `dt` seconds (at least one substep; the last substep is shortened so the time is exact). Throws std::invalid_argument for a non-finite or
    // non-positive dt, a non-finite input, or a chassis outside the collision envelope.
    // `surfaces` (optional, used only during the call, so it need not outlive it) says what each primitive of `world` is made of: the tire's friction circle is scaled by the contact surface's
    // `tire_grip` and the rolling resistance by its `rolling_resistance` (physics_material.hpp). Without it every surface is Default (both factors 1), which is the behavior before surfaces existed.
    void step(const VehicleInput& input, float dt, const StaticCollision& world, QueryScratch& scratch, const SurfaceMap* surfaces = nullptr);

    [[nodiscard]] const VehicleState& state() const noexcept { return state_; }
    [[nodiscard]] const VehicleConfig& config() const noexcept { return config_; }
    // Recorded impacts since the last call.
    [[nodiscard]] std::vector<VehicleImpact> take_impacts();
    // The chassis box at the current pose.
    [[nodiscard]] OrientedBox chassis() const;
    // Moves the body by `shift` (an origin migration): velocities, orientation and wheel state are kept.
    void migrate_origin(Vec3 shift) noexcept;
    // Zeroes the motion in place (a parked vehicle) without moving it.
    void park() noexcept;
    // The chassis' axes in the world: forward (+X), left (+Y), up (+Z).
    [[nodiscard]] std::array<Vec3, 3> axes() const noexcept;
    // Kinetic energy of the body (translation plus rotation), J.
    [[nodiscard]] double kinetic_energy() const noexcept;

private:
    struct Body {
        double p[3], q[4], v[3], w[3];
    };
    void substep_once(const VehicleInput& input, double dt, const StaticCollision& world, QueryScratch& scratch, const SurfaceMap* surfaces);
    void publish() noexcept;

    VehicleConfig config_;
    Body body_{};
    VehicleState state_;
    double inertia_[3]{};   // principal moments about the center of mass, body frame
    std::vector<VehicleImpact> impacts_;
    std::vector<StaticOrientedBoxOverlap> overlaps_;
    std::uint64_t substep_count_ = 0;
    std::array<double, 4> compression_{};
    std::array<double, 4> spin_{};
    int gear_ = 1;
    double shift_timer_ = 0.0;
    double engine_rpm_ = 0.0;
    std::array<double, 4> steer_{};
    std::array<double, 4> grip_limit_{};  // each wheel's friction limit in the last substep, N (0 in the air): what the differential shares against
    std::array<double, 4> drive_{}, slip_{};
    std::array<Vec3, 4> contact_point_{}, contact_normal_{};
};

} // namespace saige
