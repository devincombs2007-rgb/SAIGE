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
// What this is not: no tire deformation, no suspension geometry beyond a vertical spring, no anti-roll bar, no gearbox, no damage model (the host reads the
// impact events), no vehicle-vehicle contact, and one contact point per primitive. Everything is deterministic for the same inputs and build.
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
};

struct VehicleWheelState {
    bool in_contact{};
    float compression{};     // meters of spring compression (0 when drooped)
    float steer_angle{};     // rad
    float spin{};            // accumulated rotation about the axle, rad (forward motion is positive), for the wheel's visual
    Vec3 contact_point;
    Vec3 contact_normal;
    float load{};            // N pushed through the spring, last substep
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
    std::array<double, 4> steer_{};
    std::array<Vec3, 4> contact_point_{}, contact_normal_{};
};

} // namespace saige
