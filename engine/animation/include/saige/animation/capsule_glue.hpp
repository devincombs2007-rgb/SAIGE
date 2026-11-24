// SAIGE animation: the glue between the capsule character controller / static collision (Core) and the animation controllers. Header only, so the animation library itself does not depend on Core.
//
//  * `locomotion_input` turns the capsule's state into what the locomotion controller needs: its real horizontal speed, whether it is grounded, and its vertical speed. The animation follows the
//    physics, not the input, so a character pushing into a wall does not run on the spot.
//  * `ground_query` and `obstacle_query` answer the IK functions from a StaticCollision by ray casts.
#pragma once

#include "saige/animation/ik.hpp"
#include "saige/animation/locomotion.hpp"
#include "saige/capsule_character.hpp"
#include "saige/static_collision.hpp"

#include <cmath>
#include <optional>

namespace saige::anim {

[[nodiscard]] inline LocomotionInput locomotion_input(const CapsuleCharacterState& s) {
    LocomotionInput in;
    in.speed = std::sqrt(s.velocity.x * s.velocity.x + s.velocity.y * s.velocity.y);
    in.grounded = s.grounded;
    in.vertical_speed = s.velocity.z;
    return in;
}

// The ground under a point: a ray straight down from `up` meters above the reference height, for `up + down` meters. Positions are the collision's (chunk-local) meters. The collision must outlive the query.
[[nodiscard]] inline GroundQuery ground_query(const StaticCollision& world, float up = 0.7f, float down = 1.2f) {
    return [&world, up, down](float x, float y, float reference_z, GroundSample& out) {
        const std::optional<RayHit> hit = world.raycast(Vec3{x, y, reference_z + up}, Vec3{0.0f, 0.0f, -1.0f}, up + down, RayMode::Surface);
        if (!hit) return false;
        out.height = hit->point.z;
        out.normal = {hit->normal.x, hit->normal.y, hit->normal.z};
        if (out.normal.z < 0.0f) out.normal = {-out.normal.x, -out.normal.y, -out.normal.z};  // triangles are two-sided: the normal faces the ray, which is up for ground
        return true;
    };
}

[[nodiscard]] inline ObstacleQuery obstacle_query(const StaticCollision& world) {
    return [&world](render::Float3 from, render::Float3 to, render::Float3& hit) {
        const render::Float3 d = to - from;
        const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < 1e-5f) return false;
        const std::optional<RayHit> h = world.raycast(Vec3{from.x, from.y, from.z}, Vec3{d.x, d.y, d.z}, len, RayMode::Surface);
        if (!h) return false;
        if (std::fabs(h->normal.z) > 0.6f) return false;  // ground the character can stand on (a ramp, a step top), not a wall: ground IK deals with that
        hit = {h->point.x, h->point.y, h->point.z};
        return true;
    };
}

} // namespace saige::anim
