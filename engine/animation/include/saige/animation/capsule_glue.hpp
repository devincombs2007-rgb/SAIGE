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
    if (action.vertical()) {
        d.z = v.up;
        d.vertical = true;
    }
    return d;
}

// How much of the commanded travel of one step the capsule covered, in [0, 1]: the displacement of the feet along the commanded direction (in 3D for a vertical drive) over the commanded distance. 1 when nothing was commanded
// (so a pause in a clip is not a stall). `before` and `after` are the feet around the step; the drive and dt are what was given to it.
[[nodiscard]] inline float drive_progress(const Vec3& before, const Vec3& after, const CapsuleDrive& drive, float dt) {
    const float cx = drive.x * dt, cy = drive.y * dt, cz = drive.vertical ? drive.z * dt : 0.0f;
    const float commanded = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (!drive.active || commanded < 1e-5f) return 1.0f;
    const float achieved = ((after.x - before.x) * cx + (after.y - before.y) * cy + (after.z - before.z) * cz) / commanded;
    return std::clamp(achieved / commanded, 0.0f, 1.0f);
}

// A ledge ahead of a character that a climb could take it onto: what the host needs to choose a climb action (its rise against the clip's `ActionClip::travel().up`) and to stand the character at the
// distance the clip expects.
struct LedgeQuery {
    float min_height = 0.45f;      // a ledge lower than this is walked or stepped up, not climbed
    float max_height = 1.25f;      // higher than this is not a ledge a climb clip can take (the rise of the clip is what really limits it)
    float reach = 2.5f;            // how far ahead of the feet to look for the face, meters
    float probe_depth = 0.30f;     // how far behind the face to look for the top
    float headroom = 1.8f;         // clear height needed above the top for the character to stand there
    float face_height_step = 0.10f;  // spacing of the forward probes up the face
};
struct LedgeInfo {
    bool found = false;
    float distance = 0.0f;   // horizontal, from the feet to the face
    float height = 0.0f;     // of the top above the feet
    float headroom = 0.0f;   // clear height above the top (capped at the query's headroom)
    Vec3 top{};              // a point on the top, just behind the face
    Vec3 face_normal{};      // the face's unit normal in the horizontal plane (pointing out of the ledge toward the character; z is 0)
};

// Looks along `yaw` (radians counter-clockwise about +Z from +X) from the feet: the lowest forward ray above the step height that hits an upright face gives the distance, a ray straight down behind
// the face gives the top, and one straight up from the top gives the headroom. Not found when there is no face within reach, the top is lower than `min_height` or higher than `max_height`, the top is
// not a surface to stand on (steeper than about 45 degrees), or there is less than `headroom` above it. The collision must outlive the call.
[[nodiscard]] inline LedgeInfo find_ledge(const StaticCollision& world, Vec3 feet, float yaw, const LedgeQuery& q = {}) {
    LedgeInfo info;
    const float cx = std::cos(yaw), cy = std::sin(yaw);
    const Vec3 dir{cx, cy, 0.0f};
    // The lowest probe above the step height that meets an upright face.
    std::optional<RayHit> face;
    for (float h = q.min_height; h <= q.max_height + 0.5f && !face; h += q.face_height_step) {
        const auto hit = world.raycast(Vec3{feet.x, feet.y, feet.z + h}, dir, q.reach, RayMode::Surface);
        if (hit && std::fabs(hit->normal.z) < 0.35f) face = hit;
    }
    if (!face) return info;
    const float distance = static_cast<float>(face->distance);
    // The top: straight down just behind the face, from above the highest ledge looked for.
    const float ax = feet.x + cx * (distance + q.probe_depth), ay = feet.y + cy * (distance + q.probe_depth);
    const float from_z = feet.z + q.max_height + 0.6f;
    const auto top = world.raycast(Vec3{ax, ay, from_z}, Vec3{0, 0, -1}, from_z - feet.z, RayMode::Surface);
    if (!top || top->normal.z < 0.7f) return info;
    const float height = top->point.z - feet.z;
    if (height < q.min_height || height > q.max_height) return info;
    // Headroom: straight up from the top.
    float clear = q.headroom;
    if (const auto ceiling = world.raycast(Vec3{ax, ay, top->point.z + 0.02f}, Vec3{0, 0, 1}, q.headroom, RayMode::Surface)) clear = static_cast<float>(ceiling->distance);
    if (clear < q.headroom - 1e-4f) return info;
    info.found = true;
    info.distance = distance;
    info.height = height;
    info.headroom = clear;
    info.top = top->point;
    {
        const float nx = face->normal.x, ny = face->normal.y, nl = std::sqrt(nx * nx + ny * ny);
        info.face_normal = nl > 1e-5f ? Vec3{nx / nl, ny / nl, 0.0f} : Vec3{-cx, -cy, 0.0f};
    }
    return info;
}

// What it takes to stand a character right for a climb: facing the ledge's face square on, `stand_distance` from it. `ledge_alignment` gives, for the current heading and distance, the turn to command (radians per
// second, bounded by `max_turn_rate`) and the speed toward the face (positive forward, negative backing off, bounded by `max_speed`), both easing to zero as the error does, and says when the character is aligned
// (within `yaw_tolerance` and `distance_tolerance`). It needs the ledge from `find_ledge` (looked for along the heading, so a face that is oblique is found and then squared up to). Pure and deterministic.
struct LedgeAlignment {
    float turn_rate = 0.0f;   // rad/s, positive counter-clockwise
    float speed = 0.0f;       // m/s along the heading (the distance to the face is measured along the heading, which squares up as the turn completes)
    bool aligned = false;
    float yaw_error = 0.0f;   // rad, the turn still needed
    float distance_error = 0.0f;  // m, the distance still to cover (positive: too far)
};
struct LedgeAlignConfig {
    float max_turn_rate = 4.0f;
    float max_speed = 1.2f;
    float turn_gain = 6.0f;      // 1/s
    float speed_gain = 3.0f;     // 1/s
    float yaw_tolerance = 0.03f;       // rad
    float distance_tolerance = 0.03f;  // m
};
[[nodiscard]] inline LedgeAlignment ledge_alignment(const LedgeInfo& ledge, float yaw, float stand_distance, const LedgeAlignConfig& c = {}) {
    LedgeAlignment a;
    if (!ledge.found) return a;
    const float want = std::atan2(-ledge.face_normal.y, -ledge.face_normal.x);  // facing into the face
    float err = want - yaw;
    while (err > 3.14159265f) err -= 6.2831853f;
    while (err < -3.14159265f) err += 6.2831853f;
    a.yaw_error = err;
    // The distance along the heading is what find_ledge measured; square on, it is the perpendicular distance times 1/cos(error).
    a.distance_error = ledge.distance - stand_distance;
    a.turn_rate = std::clamp(err * c.turn_gain, -c.max_turn_rate, c.max_turn_rate);
    a.speed = std::clamp(a.distance_error * c.speed_gain, -c.max_speed, c.max_speed);
    a.aligned = std::fabs(err) <= c.yaw_tolerance && std::fabs(a.distance_error) <= c.distance_tolerance;
    return a;
}

// find_ledge along several headings: `steps` headings spread over +-`arc` radians about `yaw` (the centre one first, then alternating sides outward), the nearest face wins; `found_yaw` is the heading it was found on. For a
// character that is near a ledge but not facing it, so the alignment can turn it toward the ledge.
[[nodiscard]] inline LedgeInfo find_ledge_scan(const StaticCollision& world, Vec3 feet, float yaw, float arc, int steps, float& found_yaw, const LedgeQuery& q = {}) {
    LedgeInfo best;
    found_yaw = yaw;
    for (int k = 0; k < steps; ++k) {
        const int turn = (k + 1) / 2 * ((k % 2) ? 1 : -1);
        const float h = yaw + arc * static_cast<float>(turn) / static_cast<float>(std::max(1, steps / 2));
        const LedgeInfo l = find_ledge(world, feet, h, q);
        if (l.found && (!best.found || l.distance < best.distance - 1e-4f)) {
            best = l;
            found_yaw = h;
        }
    }
    return best;
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
