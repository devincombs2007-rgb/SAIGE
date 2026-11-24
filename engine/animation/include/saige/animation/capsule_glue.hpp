// SAIGE animation: the glue between the capsule character controller / static collision (Core) and the animation controllers. Header only, so the animation library itself does not depend on Core.
//
//  * `locomotion_input` turns the capsule's state into what the locomotion controller needs: its real horizontal speed, whether it is grounded, and its vertical speed. The animation follows the
//    physics, not the input, so a character pushing into a wall does not run on the spot.
//  * `capsule_drive` and `drive_progress` connect a root-motion action (action.hpp) to the capsule: the action's velocity in the character's frame becomes `CapsuleInput::drive`, and the share of the
//    commanded travel the capsule really covered goes back to the action.
//  * `ground_query` and `obstacle_query` answer the IK functions from a StaticCollision by ray casts.
#pragma once

#include "saige/animation/action.hpp"
#include "saige/animation/ik.hpp"
#include "saige/animation/locomotion.hpp"
#include "saige/capsule_character.hpp"
#include "saige/static_collision.hpp"

#include <algorithm>
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

// The action's velocity in the character's own frame (forward, left) as a world-axes drive for the capsule, for a character facing `yaw` radians counter-clockwise about +Z from +X (the axes of the
// capsule's input). An action that is not travelling gives an inactive drive.
[[nodiscard]] inline CapsuleDrive capsule_drive(const ActionPlayer& action, float dt, float yaw) {
    CapsuleDrive d;
    const PlanarVelocity v = action.command(dt);
    if (!action.travelling()) return d;
    const float c = std::cos(yaw), s = std::sin(yaw);
    d.active = true;
    d.x = v.forward * c - v.left * s;
    d.y = v.forward * s + v.left * c;
    return d;
}

// How much of the commanded travel of one step the capsule covered, in [0, 1]: the displacement of the feet along the commanded direction over the commanded distance. 1 when nothing was commanded
// (so a pause in a clip is not a stall). `before` and `after` are the feet around the step; the drive and dt are what was given to it.
[[nodiscard]] inline float drive_progress(const Vec3& before, const Vec3& after, const CapsuleDrive& drive, float dt) {
    const float cx = drive.x * dt, cy = drive.y * dt;
    const float commanded = std::hypot(cx, cy);
    if (!drive.active || commanded < 1e-5f) return 1.0f;
    const float achieved = ((after.x - before.x) * cx + (after.y - before.y) * cy) / commanded;
    return std::clamp(achieved / commanded, 0.0f, 1.0f);
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
