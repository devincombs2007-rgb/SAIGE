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
// (a bouncy object stays bouncy on a dead floor only partly, and the maximum is the common rule). Both are symmetric in their arguments.

#include "saige/static_collision.hpp"

#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace saige {

enum class Surface : std::uint8_t { Default, Asphalt, AsphaltWet, Concrete, Gravel, Dirt, Grass, Sand, Snow, Ice, Rubber, Count };

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

// The traction under a character's feet: a ray straight down from `up` above them for `up + down` meters, and the material of the primitive it hits; 1 when there is nothing underfoot (the character is
// airborne, where traction does not matter). The collision and the map must describe the same primitives the character collides with (the same arrays, in the same order).
[[nodiscard]] inline float ground_traction(const StaticCollision& world, const SurfaceMap& map, Vec3 feet, float up = 0.2f, float down = 0.4f) {
    const std::optional<RayHit> hit = world.raycast(Vec3{feet.x, feet.y, feet.z + up}, Vec3{0.0f, 0.0f, -1.0f}, up + down, RayMode::Surface);
    return hit ? foot_traction(map.material_at(hit->primitive)) : 1.0f;
}

} // namespace saige
