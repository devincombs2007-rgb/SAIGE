#pragma once

// Physical surface materials: what a tire, a body or a falling object meets depends on what it touches, so the collision world can say which material each primitive is.
//
// `StaticCollision` itself is geometry only and is unchanged. A `SurfaceMap` is a parallel table with one `Surface` per box and per triangle (by the primitive ids the collision reports), and a
// consumer that cares looks the contact's primitive up in it. The vehicle does (`Vehicle::step`'s optional `surfaces` argument); passing none is exactly the behavior from before this header
// existed (every surface is `Surface::Default`, whose grip multiplier is 1 and whose rolling-resistance multiplier is 1).
//
// The numbers are textbook ranges for dry conditions (tire-road friction and rolling-resistance coefficients as tabulated in vehicle-dynamics texts such as Wong, "Theory of Ground
// Vehicles", and Gillespie, "Fundamentals of Vehicle Dynamics"), expressed relative to dry asphalt so that `VehicleConfig::tire_friction` stays the dry-asphalt value of the tire. They are
// approximations for a believable difference between surfaces, not measurements of any particular tire; a game replaces the table for its own surfaces.
//
// Combining two materials: friction by the geometric mean (the usual game-physics rule: either surface being slippery makes the pair slippery, and it is symmetric); restitution by the larger
// (a bouncy object stays bouncy on a dead floor only partly, and the maximum is the common rule). Both are symmetric in their arguments. `contact_velocity` applies them to a falling object (debris, a dropped
// prop) meeting a surface.

#include "saige/static_collision.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace saige {

enum class Surface : std::uint8_t { Default, Asphalt, AsphaltWet, Concrete, Gravel, Dirt, Grass, Sand, Snow, Ice, Rubber, Count };

// What a breakable object is made of (include/saige/destruction.hpp): it decides how it sounds and what it throws when it breaks, not how it collides (that is the Surface).
enum class BreakMaterial : std::uint8_t { Wood, Glass, Metal, Stone, Plastic };

struct PhysicsMaterial {
    const char* name;
    float tire_grip;            // multiplier of the tire's friction circle (dry asphalt = 1)
    float rolling_resistance;   // multiplier of the vehicle's rolling resistance (dry asphalt = 1)
    float friction;             // body and object friction coefficient (Coulomb)
    float restitution;          // 0 dead .. 1 perfectly elastic
    float density;              // kg per cubic meter of the solid, for objects made of it
};

namespace detail {
// index = Surface; the order is part of the contract (the static_assert below checks the count). Namespace scope, so the references handed out have static storage.
inline constexpr PhysicsMaterial physics_material_table[] = {
    {"default",     1.00f, 1.0f, 0.60f, 0.20f, 2400.0f},
    {"asphalt",     1.00f, 1.0f, 0.65f, 0.20f, 2300.0f},
    {"asphalt_wet", 0.70f, 1.0f, 0.45f, 0.15f, 2300.0f},
    {"concrete",    0.95f, 1.0f, 0.60f, 0.20f, 2400.0f},
    {"gravel",      0.62f, 3.0f, 0.55f, 0.15f, 1800.0f},
    {"dirt",        0.68f, 3.5f, 0.55f, 0.10f, 1600.0f},
    {"grass",       0.55f, 5.0f, 0.50f, 0.10f, 1100.0f},
    {"sand",        0.50f, 8.0f, 0.50f, 0.05f, 1600.0f},
    {"snow",        0.25f, 4.0f, 0.20f, 0.05f,  400.0f},
    {"ice",         0.10f, 1.0f, 0.05f, 0.10f,  917.0f},
    {"rubber",      0.90f, 1.5f, 0.90f, 0.65f, 1100.0f},  // a rubber mat or a tire barrier: grippy and lively
};
static_assert(sizeof(physics_material_table) / sizeof(physics_material_table[0]) == static_cast<std::size_t>(Surface::Count), "one material per Surface");
} // namespace detail

// An out-of-range Surface (a corrupt byte from a file) reads as Default.
[[nodiscard]] constexpr const PhysicsMaterial& physics_material(Surface surface) noexcept {
    const std::size_t i = static_cast<std::size_t>(surface);
    return detail::physics_material_table[i < static_cast<std::size_t>(Surface::Count) ? i : 0];
}

[[nodiscard]] inline float combine_friction(float a, float b) noexcept { return std::sqrt(a * b); }
[[nodiscard]] constexpr float combine_restitution(float a, float b) noexcept { return a > b ? a : b; }

// How well a character's feet grip a material relative to dry asphalt, in [0.02, 1] (CapsuleInput::traction): its body friction over asphalt's.
[[nodiscard]] inline float foot_traction(const PhysicsMaterial& m) noexcept {
    const float t = m.friction / physics_material(Surface::Asphalt).friction;
    return t < 0.02f ? 0.02f : (t > 1.0f ? 1.0f : t);
}

// One Surface per box and per triangle of a StaticCollision, indexed by the primitive ids the collision reports.
class SurfaceMap {
public:
    SurfaceMap() = default;
    SurfaceMap(std::size_t boxes, std::size_t triangles, Surface fill = Surface::Default) : boxes_(boxes, fill), triangles_(triangles, fill) {}
    // The right size for `world`, everything `fill`.
    explicit SurfaceMap(const StaticCollision& world, Surface fill = Surface::Default) : SurfaceMap(world.box_count(), world.triangle_count(), fill) {}

    // Throws std::out_of_range for a primitive the map does not cover.
    void set(const StaticPrimitive& primitive, Surface surface) { slot(primitive) = surface; }
    // The surface of a primitive; Default for one the map does not cover (a map built for another snapshot must not crash a query, and Default is the old behavior).
    [[nodiscard]] Surface at(const StaticPrimitive& primitive) const noexcept {
        const auto& v = primitive.kind == StaticPrimitive::Kind::Box ? boxes_ : triangles_;
        return primitive.index < v.size() ? v[primitive.index] : Surface::Default;
    }
    [[nodiscard]] const PhysicsMaterial& material_at(const StaticPrimitive& primitive) const noexcept { return physics_material(at(primitive)); }
    [[nodiscard]] std::size_t box_count() const noexcept { return boxes_.size(); }
    [[nodiscard]] std::size_t triangle_count() const noexcept { return triangles_.size(); }

private:
    Surface& slot(const StaticPrimitive& primitive) {
        auto& v = primitive.kind == StaticPrimitive::Kind::Box ? boxes_ : triangles_;
        if (primitive.index >= v.size()) throw std::out_of_range("SurfaceMap::set: primitive outside the map");
        return v[primitive.index];
    }
    std::vector<Surface> boxes_, triangles_;
};

// The velocity of a small object after it meets a surface: the part of the velocity into the surface is reflected and scaled by the combined restitution of the two materials, and the part along the surface
// loses the Coulomb friction impulse of the contact (mu times the change of normal velocity, never more than the tangential speed itself: friction stops sliding, it never reverses it). A normal speed
// at or below `rest_speed` is not bounced (the object stays on the surface), which stops the endless tiny bounces of a settling object. `normal` points out of the surface, to the side the object
// is on (a `RayHit` normal does: it faces the ray's origin); a velocity already leaving the surface or running along it is returned unchanged. The kinetic energy of the result is never more than before (checked by the tests); a normal that is not
// finite or has a length far from 1 is a caller error and gives the input velocity back.
[[nodiscard]] inline Vec3 contact_velocity(Vec3 velocity, Vec3 normal, const PhysicsMaterial& object, const PhysicsMaterial& surface, float rest_speed = 0.35f) noexcept {
    const float nl = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (!std::isfinite(nl) || !(nl > 0.5f) || !(nl < 2.0f) || !std::isfinite(velocity.x) || !std::isfinite(velocity.y) || !std::isfinite(velocity.z)) return velocity;
    const Vec3 unit{normal.x / nl, normal.y / nl, normal.z / nl};
    const float vn = velocity.x * unit.x + velocity.y * unit.y + velocity.z * unit.z;
    if (!(vn < 0.0f)) return velocity;  // along the surface, or leaving it
    const float into = -vn;
    const float e = into > rest_speed ? combine_restitution(object.restitution, surface.restitution) : 0.0f;
    const float mu = combine_friction(object.friction, surface.friction);
    const Vec3 tangent{velocity.x - vn * unit.x, velocity.y - vn * unit.y, velocity.z - vn * unit.z};
    const float ts = std::sqrt(tangent.x * tangent.x + tangent.y * tangent.y + tangent.z * tangent.z);
    const float kept = ts > 0.0f ? std::max(0.0f, ts - mu * (1.0f + e) * into) / ts : 0.0f;
    const float out = e * into;
    return {tangent.x * kept + unit.x * out, tangent.y * kept + unit.y * out, tangent.z * kept + unit.z * out};
}

// The same for a contact the collision reported: the surface is the map's material for the primitive that was hit.
[[nodiscard]] inline Vec3 contact_velocity(Vec3 velocity, const RayHit& hit, const PhysicsMaterial& object, const SurfaceMap& map, float rest_speed = 0.35f) noexcept {
    return contact_velocity(velocity, hit.normal, object, map.material_at(hit.primitive), rest_speed);
}

// The traction under a character's feet: a ray straight down from `up` above them for `up + down` meters, and the material of the primitive it hits; 1 when there is nothing underfoot (the character is
// airborne, where traction does not matter). The collision and the map must describe the same primitives the character collides with (the same arrays, in the same order).
[[nodiscard]] inline float ground_traction(const StaticCollision& world, const SurfaceMap& map, Vec3 feet, float up = 0.2f, float down = 0.4f) {
    const std::optional<RayHit> hit = world.raycast(Vec3{feet.x, feet.y, feet.z + up}, Vec3{0.0f, 0.0f, -1.0f}, up + down, RayMode::Surface);
    return hit ? foot_traction(map.material_at(hit->primitive)) : 1.0f;
}

} // namespace saige
