#include "saige/animation/ik.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::anim {

using render::Float3;
using render::Quat;

namespace {

// The shortest rotation taking the direction `a` to `b` (both need not be unit).
Quat from_to(Float3 a, Float3 b) {
    const float la = length(a), lb = length(b);
    if (la < 1e-8f || lb < 1e-8f) return {0, 0, 0, 1};
    a = a * (1.0f / la);
    b = b * (1.0f / lb);
    const float d = std::clamp(dot(a, b), -1.0f, 1.0f);
    if (d > 0.999999f) return {0, 0, 0, 1};
    Float3 axis = render::cross(a, b);
    if (d < -0.999999f) {  // opposite: any axis perpendicular to a
        axis = std::fabs(a.x) < 0.9f ? render::cross(a, Float3{1, 0, 0}) : render::cross(a, Float3{0, 1, 0});
    }
    return render::from_axis_angle(axis, std::acos(d));
}

// Sets joint j's local rotation so that its world rotation becomes `world_rot`, given the world rotation of its parent.
void set_world_rotation(const gfx::Skin& skin, gfx::Pose& pose, const std::vector<JointWorld>& world, std::size_t j, Quat world_rot) {
    const JointWorld parent = parent_world(skin, world, j);
    Quat parent_rot = parent.rotation;
    if (skin.parent[j] < 0) {
        const render::Mat4& b = skin.base[j];
        const auto col = [&](int c) { return render::normalize(Float3{b.at(0, c), b.at(1, c), b.at(2, c)}); };
        parent_rot = render::from_basis(col(0), col(1), col(2));
    }
    pose.local[j].rotation = render::normalize(render::conjugate(parent_rot) * world_rot);
}

} // namespace

TwoBoneResult solve_two_bone(const gfx::Skin& skin, gfx::Pose& pose, std::size_t upper, std::size_t lower, std::size_t end, Float3 target, Float3 pole, float weight) {
    TwoBoneResult result;
    if (upper >= skin.joint_count() || lower >= skin.joint_count() || end >= skin.joint_count() || pose.local.size() != skin.joint_count()) throw std::invalid_argument("solve_two_bone: joint out of range");
    if (static_cast<std::size_t>(skin.parent[lower]) != upper || static_cast<std::size_t>(skin.parent[end]) != lower) throw std::invalid_argument("solve_two_bone: the joints are not a chain upper -> lower -> end");
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) throw std::invalid_argument("solve_two_bone: non-finite target");
    weight = std::clamp(weight, 0.0f, 1.0f);
    const gfx::Pose original = pose;
    std::vector<JointWorld> w = joint_world(skin, pose);
    const Float3 A = w[upper].position, B = w[lower].position, C = w[end].position;
    const float a = length(B - A), b = length(C - B);
    if (a < 1e-5f || b < 1e-5f) return result;
    result.solved = true;
    Float3 to_target = target - A;
    float d = length(to_target);
    if (d < 1e-6f) {
        to_target = C - A;
        d = std::max(length(to_target), 1e-6f);
    }
    const Float3 u = to_target * (1.0f / d);
    const float reach_min = std::fabs(a - b) + 1e-4f, reach_max = a + b - 1e-4f;
    const float dc = std::clamp(d, reach_min, reach_max);
    result.reached = d <= reach_max && d >= reach_min;
    // The middle joint: at distance a from A and b from the (reachable) target, on the side of the pole.
    const float cos_a = std::clamp((a * a + dc * dc - b * b) / (2.0f * a * dc), -1.0f, 1.0f);
    const float sin_a = std::sqrt(std::max(0.0f, 1.0f - cos_a * cos_a));
    Float3 side = pole - u * dot(pole, u);
    if (length(side) < 1e-5f) side = (B - A) - u * dot(B - A, u);  // pole along the axis: keep the current bend direction
    if (length(side) < 1e-5f) side = Float3{0, 0, 1} - u * dot(Float3{0, 0, 1}, u);
    side = side * (1.0f / std::max(length(side), 1e-6f));
    const Float3 mid = A + u * (a * cos_a) + side * (a * sin_a);
    const Float3 T = A + u * dc;
    // Upper: turn so that its child direction points at the new middle joint.
    set_world_rotation(skin, pose, w, upper, render::normalize(from_to(B - A, mid - A) * w[upper].rotation));
    w = joint_world(skin, pose);
    // Lower: turn so that the end joint points at the target from the (moved) middle joint.
    const Float3 B2 = w[lower].position, C2 = w[end].position;
    set_world_rotation(skin, pose, w, lower, render::normalize(from_to(C2 - B2, T - B2) * w[lower].rotation));
    if (weight < 1.0f) {
        for (const std::size_t j : {upper, lower}) pose.local[j].rotation = render::slerp(original.local[j].rotation, pose.local[j].rotation, weight);
    }
    w = joint_world(skin, pose);
    result.end_error = length(w[end].position - T);
    return result;
}

void adapt_feet_to_ground(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, gfx::Pose& pose, const render::Transform& root, const GroundQuery& ground, const FootIkConfig& cfg,
                          FootIkState& state, float dt) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) throw std::invalid_argument("adapt_feet_to_ground: bad dt");
    const auto to_world = [&](Float3 p) { return root.position + render::rotate(root.rotation, p); };
    struct Foot {
        Bone upper, lower, foot, toes;
    };
    const Foot feet[2] = {{Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot, Bone::LeftToes}, {Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot, Bone::RightToes}};
    std::vector<JointWorld> w = joint_world(skin, pose);
    struct Plan {
        bool active = false;
        float dz = 0.0f;       // how far the foot must move vertically to sit on the ground (positive: up)
        float fade = 0.0f;
        GroundSample g;
    };
    Plan plan[2];
    float pelvis_target = 0.0f;
    bool any = false;
    for (int f = 0; f < 2; ++f) {
        const std::size_t contact = static_cast<std::size_t>(map.has(feet[f].toes) ? map[feet[f].toes] : map[feet[f].foot]);
        const Float3 cw = to_world(w[contact].position);
        // Where the animation puts the sole relative to the character's own floor (the root's height): that is its intent. Planted when it is on the floor.
        const float intent = cw.z - rig.contact_offset - root.position.z;
        float fade = 1.0f;
        if (intent > cfg.plant_height) fade = 1.0f - std::clamp((intent - cfg.plant_height) / cfg.blend_height, 0.0f, 1.0f);
        if (fade <= 0.0f) continue;
        GroundSample g;
        if (!ground(cw.x, cw.y, root.position.z, g)) continue;
        plan[f].active = true;
        plan[f].fade = fade;
        plan[f].g = g;
        plan[f].dz = (g.height - root.position.z) * fade;
        pelvis_target = any ? std::min(pelvis_target, plan[f].dz) : plan[f].dz;
        any = true;
    }
    pelvis_target = any ? std::clamp(pelvis_target, -cfg.max_pelvis_drop, cfg.max_pelvis_rise) : 0.0f;
    const float k = cfg.pelvis_smoothing > 0.0f ? 1.0f - std::exp(-dt / cfg.pelvis_smoothing) : 1.0f;
    state.pelvis_offset += (pelvis_target - state.pelvis_offset) * k;

    // Move the pelvis: the whole body follows, the feet are brought back to their targets below.
    const std::size_t hips = static_cast<std::size_t>(map[Bone::Hips]);
    {
        const JointWorld parent = parent_world(skin, w, hips);
        Quat parent_rot = parent.rotation;
        if (skin.parent[hips] < 0) {
            const render::Mat4& b = skin.base[hips];
            const auto col = [&](int c) { return render::normalize(Float3{b.at(0, c), b.at(1, c), b.at(2, c)}); };
            parent_rot = render::from_basis(col(0), col(1), col(2));
        }
        const Float3 world_shift = render::rotate(render::conjugate(root.rotation), Float3{0, 0, state.pelvis_offset});  // model-space up is the world's up turned by the heading (yaw only)
        pose.local[hips].position = pose.local[hips].position + render::rotate(render::conjugate(parent_rot), world_shift);
    }
    const std::vector<JointWorld> before = w;
    w = joint_world(skin, pose);
    for (int f = 0; f < 2; ++f) {
        if (!plan[f].active) continue;
        const std::size_t ankle = static_cast<std::size_t>(map[feet[f].foot]);
        const std::size_t contact = static_cast<std::size_t>(map.has(feet[f].toes) ? map[feet[f].toes] : map[feet[f].foot]);
        // The foot keeps the orientation the animation gave it, tilted toward the ground's normal as far as the limit allows (and as far as the foot is planted).
        const Float3 n_model = render::rotate(render::conjugate(root.rotation), plan[f].g.normal);
        Quat tilt = from_to(Float3{0, 0, 1}, n_model);
        const float angle = 2.0f * std::acos(std::min(1.0f, std::fabs(tilt.w)));
        if (angle > cfg.max_slope_tilt) tilt = render::slerp(Quat{0, 0, 0, 1}, tilt, cfg.max_slope_tilt / angle);
        tilt = render::slerp(Quat{0, 0, 0, 1}, tilt, plan[f].fade);
        // Where the contact point must end up: where the animation had it, moved vertically onto the ground. The tilt turns the foot about the ankle and so moves the contact point; the ankle target
        // accounts for that.
        const Float3 offset = before[contact].position - before[ankle].position;  // ankle -> contact point, in the animation's foot orientation
        const Float3 desired_contact = before[contact].position + Float3{0, 0, plan[f].dz};
        const Float3 ankle_target = desired_contact - render::rotate(tilt, offset);
        const Float3 pole = render::rotate(render::conjugate(root.rotation), Float3{1, 0, 0});  // knees bend forward, in model space
        solve_two_bone(skin, pose, static_cast<std::size_t>(map[feet[f].upper]), static_cast<std::size_t>(map[feet[f].lower]), ankle, ankle_target, pole);
        const std::vector<JointWorld> after = joint_world(skin, pose);
        set_world_rotation(skin, pose, after, ankle, render::normalize(tilt * before[ankle].rotation));
    }
}

} // namespace saige::anim

namespace saige::anim {

int keep_feet_outside_obstacles(const gfx::Skin& skin, const HumanoidMap& map, gfx::Pose& pose, const render::Transform& root, const ObstacleQuery& obstacle, const ObstacleIkConfig& cfg) {
    const auto to_world = [&](Float3 p) { return root.position + render::rotate(root.rotation, p); };
    const auto to_model = [&](Float3 p) { return render::rotate(render::conjugate(root.rotation), p - root.position); };
    struct Foot {
        Bone upper, lower, foot, toes;
    };
    const Foot feet[2] = {{Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot, Bone::LeftToes}, {Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot, Bone::RightToes}};
    int moved = 0;
    for (int f = 0; f < 2; ++f) {
        const std::vector<JointWorld> w = joint_world(skin, pose);
        const std::size_t ankle = static_cast<std::size_t>(map[feet[f].foot]);
        const std::size_t contact = static_cast<std::size_t>(map.has(feet[f].toes) ? map[feet[f].toes] : map[feet[f].foot]);
        const Float3 tip = to_world(w[contact].position);
        // From the character's axis at the foot's height to a little beyond the foot.
        const float h = std::max(tip.z, root.position.z + cfg.min_height);
        const Float3 from{root.position.x, root.position.y, h};
        Float3 dir{tip.x - from.x, tip.y - from.y, 0.0f};
        const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
        if (len < 1e-4f) continue;
        dir = dir * (1.0f / len);
        const Float3 to = from + dir * (len + cfg.margin);
        Float3 hit;
        if (!obstacle(from, to, hit)) continue;
        // How far the foot is past the allowed distance along the segment.
        const float allowed = std::sqrt((hit.x - from.x) * (hit.x - from.x) + (hit.y - from.y) * (hit.y - from.y)) - cfg.margin;
        const float over = len - allowed;
        if (over <= 0.0f) continue;
        const Float3 shift = dir * (-over);
        const Float3 ankle_target = to_model(to_world(w[ankle].position) + shift);
        const Float3 pole = render::rotate(render::conjugate(root.rotation), Float3{1, 0, 0});
        solve_two_bone(skin, pose, static_cast<std::size_t>(map[feet[f].upper]), static_cast<std::size_t>(map[feet[f].lower]), ankle, ankle_target, pole);
        ++moved;
    }
    return moved;
}

} // namespace saige::anim
