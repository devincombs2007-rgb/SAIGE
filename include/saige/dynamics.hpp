#pragma once

// The SAIGE dynamic rigid-body interface (issue #43, physics). A world of dynamic, kinematic and static bodies with constraints, contact materials, continuous collision detection and ray casts, behind an interface SAIGE
// owns so that the engine never names a library. What it does NOT replace: the capsule character controller, the raycast vehicle, the water module, the physical material table and StaticCollision all stay
// SAIGE's own; this is for what they do not do (stacks, piles, ragdolls, props, chains). A backend is optional: `make_dynamics_world` says which are compiled in and returns null for one that is not (the
// Jolt backend is built only with -DSAIGE_PHYSICS_JOLT=ON, see engine/physics_jolt).
//
// Conventions. SI units, SAIGE axes (+X forward, +Y left, +Z up), gravity along -Z. Positions are chunk-local meters (the same frame as StaticCollision and the vehicle): the world is built in one chunk's frame and
// `shift_origin` moves everything when the host migrates its origin, so a backend never sees global coordinates (large worlds keep their double-precision global position outside, as the architecture says).
// Bodies are identified by `BodyId` (index and generation: a stale id is rejected, not reused). Materials are SAIGE `Surface` values, so a body and a tire and a footstep agree on what a surface is; the contact
// pair's friction and restitution follow SAIGE's rule (friction by the geometric mean, restitution by the larger), whatever the library's own default is.
//
// Determinism. A world stepped with the same bodies, inputs and fixed step gives the same state tick for tick on the same build and thread count (backends say whether they also promise it across thread counts in
// `DynamicsInfo`). Not promised across compilers, platforms or library versions.

#include "saige/physics_material.hpp"
#include "saige/static_collision.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace saige {

struct DVec {
    double x = 0.0, y = 0.0, z = 0.0;
};
struct DQuat {
    double x = 0.0, y = 0.0, z = 0.0, w = 1.0;
};

struct BodyId {
    std::uint32_t index = 0xffffffffu;
    std::uint32_t generation = 0;
    [[nodiscard]] bool valid() const noexcept { return index != 0xffffffffu; }
    bool operator==(const BodyId&) const = default;
};

enum class DShapeKind : std::uint8_t { Box, Sphere, Capsule };
enum class DMotion : std::uint8_t { Static, Kinematic, Dynamic };

struct DBodyDesc {
    DShapeKind shape = DShapeKind::Box;
    DVec half_extents{0.5, 0.5, 0.5};   // Box: half sizes. Sphere: x is the radius. Capsule: x is the radius, y the half length of the cylinder (axis along the body's Z)
    DVec position;
    DQuat rotation;
    DMotion motion = DMotion::Dynamic;
    double mass = 0.0;                   // kg; 0 means density of the material times the volume
    Surface surface = Surface::Default;  // friction and restitution from the material table; `friction` and `restitution` below override when set (>= 0)
    float friction = -1.0f;
    float restitution = -1.0f;
    bool continuous = false;             // continuous collision detection against everything (thin walls, fast bodies): costs time
    std::uint32_t self_collision_group = 0;  // nonzero: bodies of one group never collide with each other (up to 64 per group), only with everything else: the limbs of a ragdoll
    float linear_damping = 0.0f;         // per second; SAIGE bodies have none by default (a library's own default of 0.05 would be a hidden drag)
    float angular_damping = 0.0f;
    DVec linear_velocity;
    DVec angular_velocity;
};

struct DBodyState {
    DVec position;
    DQuat rotation;
    DVec linear_velocity;
    DVec angular_velocity;
    bool active = true;                  // false: asleep
};

struct ConstraintId {
    std::uint32_t index = 0xffffffffu;
    std::uint32_t generation = 0;
    [[nodiscard]] bool valid() const noexcept { return index != 0xffffffffu; }
    bool operator==(const ConstraintId&) const = default;
};

// A joint motor: a spring-damper toward a target, solved with the constraints (implicitly, so it is stable at any stiffness and while the body is in contact). `frequency` (Hz) and `damping` (1: critical) are
// those of the spring in the joint's own motion, independent of the masses; `max_torque` (N m) caps what it can push (0: unlimited).
struct DMotor {
    double frequency = 4.0;
    double damping = 1.0;
    double max_torque = 0.0;
};

enum class DConstraintKind : std::uint8_t { Fixed, Hinge, SwingTwist, Distance };

// What a constraint carried in the last step: the magnitude of the force (N) and of the torque (N m) it applied to hold its two bodies in their relation, averaged over the last solver sub-step.
// This is how a breakable joint knows it is overloaded (include/saige/destruction.hpp). The value is zero for a backend that cannot report it (the default) and for a stale id.
struct DConstraintLoad {
    double force = 0.0;
    double torque = 0.0;
};

struct DConstraintDesc {
    DConstraintKind kind = DConstraintKind::Hinge;
    BodyId a, b;                         // b may be invalid: a is fixed to the world
    DVec pivot;                          // world space, at creation
    DVec axis{0, 0, 1};                  // hinge axis, or the twist axis of a swing-twist (world space, at creation)
    double min_angle = -3.14159265358979, max_angle = 3.14159265358979;  // hinge range, or the twist range of a swing-twist (radians)
    double swing_half_angle = 0.7;       // swing-twist: the cone half angle (radians)
    double min_distance = 0.0, max_distance = 0.0;  // Distance: the allowed range between the pivots (both pivots at the world `pivot` at creation unless `pivot_b` is set)
    DVec pivot_b;
    bool use_pivot_b = false;
};

struct DRayHit {
    bool hit = false;
    double fraction = 1.0;               // of the segment
    DVec point, normal;
    BodyId body;                         // invalid when the hit is the static collision
};

struct DynamicsInfo {
    std::string backend;                 // "jolt"
    std::string version;
    bool double_precision = false;       // the library was built with 64-bit positions
    bool multithreaded = false;
    bool deterministic_across_threads = false;
    bool continuous_collision = false;
    std::size_t max_bodies = 0;
};

struct DynamicsConfig {
    std::size_t max_bodies = 4096;
    std::size_t max_constraints = 4096;
    unsigned threads = 1;                // worker threads the backend may use (1: none)
    double gravity = 9.81;
    int collision_steps = 1;             // physics sub-steps per step() call
    int velocity_iterations = 10;
    int position_iterations = 2;
    bool allow_sleeping = true;          // bodies at rest go to sleep (cheap); off for controllers that act on bodies every step (ragdoll drives)
};

class DynamicsWorld {
public:
    virtual ~DynamicsWorld() = default;
    [[nodiscard]] virtual DynamicsInfo info() const = 0;
    // Static world geometry from a StaticCollision's boxes and triangles (the same primitives, in the same frame), with a surface per primitive; one call, replacing the earlier geometry.
    virtual void set_static_geometry(const StaticCollision& collision, const SurfaceMap* surfaces) = 0;
    // Throws std::invalid_argument for a non-finite or non-positive size, a mass, damping or velocity not finite, or a world that is full.
    virtual BodyId add_body(const DBodyDesc& desc) = 0;
    virtual void remove_body(BodyId id) = 0;
    virtual ConstraintId add_constraint(const DConstraintDesc& desc) = 0;
    // Drives a hinge or swing-twist toward the orientation of its second body relative to its first (the second body's rotation = the first's times `target`, both in body space; identity is the pose the
    // constraint was created in). A hinge follows only the part about its axis. Throws std::invalid_argument for a stale id, another kind of constraint or a bad motor.
    virtual void set_motor(ConstraintId id, DQuat target, const DMotor& motor) = 0;
    virtual void clear_motor(ConstraintId id) = 0;
    virtual void remove_constraint(ConstraintId id) = 0;
    [[nodiscard]] virtual DConstraintLoad constraint_load(ConstraintId id) const { (void)id; return {}; }  // additive in 5.8 of the interface; see DConstraintLoad
    [[nodiscard]] virtual DBodyState state(BodyId id) const = 0;
    virtual void set_velocity(BodyId id, DVec linear, DVec angular) = 0;
    virtual void add_impulse(BodyId id, DVec impulse, DVec world_point) = 0;
    // An angular impulse (N m s, world axes) about the center of mass: a torque held for dt is `torque * dt` (motors and controllers apply their torques this way, once per step).
    virtual void add_angular_impulse(BodyId id, DVec impulse) = 0;
    // Wakes a sleeping body (a body driven by a joint motor sleeps like any other once it is still, short of its target; wake it, or run the world with allow_sleeping off, while a controller is acting).
    virtual void wake(BodyId id) = 0;
    // Teleports a body (its velocity is kept; set it with set_velocity). Constraints keep their reference frames in the bodies, so a pose consistent with the joints (limb lengths) carries them along.
    virtual void set_transform(BodyId id, DVec position, DQuat rotation) = 0;
    virtual void set_kinematic_target(BodyId id, DVec position, DQuat rotation, double dt) = 0;
    [[nodiscard]] virtual std::size_t body_count() const = 0;
    // Advances by dt seconds (a fixed step of 1/60 s or shorter is the supported use). Throws for a non-finite or non-positive dt or one over 0.1 s.
    virtual void step(double dt) = 0;
    // Moves the whole world by `shift` (an origin migration): bodies, constraints and static geometry stay consistent. Static geometry must be set again for the new frame by the caller.
    virtual void shift_origin(DVec shift) = 0;
    [[nodiscard]] virtual DRayHit cast_ray(DVec from, DVec to) const = 0;
};

enum class DynamicsBackend : std::uint8_t { Jolt };

// The backends compiled into this build.
[[nodiscard]] std::vector<DynamicsBackend> available_dynamics_backends();
// A new world on the backend, or null when it is not compiled in. Throws std::invalid_argument for an invalid configuration.
[[nodiscard]] std::unique_ptr<DynamicsWorld> make_dynamics_world(DynamicsBackend backend, const DynamicsConfig& config);

} // namespace saige
