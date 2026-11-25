#include "saige/animation/seat_entry.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::anim {

using render::Float3;
using render::Quat;
using render::Transform;

namespace {

bool finite(Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
float smooth(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
float lerp(float a, float b, float t) { return a + (b - a) * t; }
float wrap_pi(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}
float yaw_of(const Transform& t) {
    const Float3 f = render::rotate(t.rotation, Float3{1, 0, 0});
    return std::atan2(f.y, f.x);
}
Float3 to_world(const Transform& vehicle, Float3 local) { return vehicle.position + render::rotate(vehicle.rotation, local); }

} // namespace

VehicleEntry::VehicleEntry(const VehicleEntryRig& rig, const VehicleEntryConfig& config) : rig_(rig), config_(config) {
    const auto positive = [](float x) { return std::isfinite(x) && x > 0.0f; };
    if (!positive(config.walk_speed) || !positive(config.turn_rate) || !positive(config.reach_time) || !positive(config.open_time) || !positive(config.enter_time) || !positive(config.settle_time) ||
        !(config.max_vehicle_speed >= 0.0f && std::isfinite(config.max_vehicle_speed))) {
        throw std::invalid_argument("VehicleEntry: the times and speeds must be finite and positive");
    }
    const float axis = length(rig.hinge_axis);
    if (!(axis > 1e-4f) || !std::isfinite(axis) || !(rig.door_max >= 0.0f && rig.door_max <= 3.0f)) throw std::invalid_argument("VehicleEntry: the hinge axis must be non-zero and the door range in [0, 3] rad");
    if (!finite(rig.hinge_point) || !finite(rig.handle) || !finite(rig.stand_point) || !finite(rig.threshold) || !finite(rig.seat_root) || !finite(rig.wheel_left) || !finite(rig.wheel_right) ||
        !finite(rig.pedal_left) || !finite(rig.pedal_right) || !finite(rig.open_step_back) || !std::isfinite(rig.seat_hips_height) || !std::isfinite(rig.stand_yaw) || !std::isfinite(rig.seat_yaw)) {
        throw std::invalid_argument("VehicleEntry: the rig has a value that is not finite");
    }
    rig_.hinge_axis = rig.hinge_axis * (1.0f / axis);
}

bool VehicleEntry::begin(const Transform& vehicle, Float3 root_position, float root_yaw, float standing_hips, float vehicle_speed) {
    if (!finite(root_position) || !std::isfinite(root_yaw) || !(standing_hips > 0.0f) || !std::isfinite(standing_hips) || !std::isfinite(vehicle_speed)) return false;
    if (vehicle_speed > config_.max_vehicle_speed) return false;
    started_ = true;
    standing_hips_ = standing_hips;
    frame_ = {};
    frame_.phase = EntryPhase::Approach;
    frame_.root = Transform{root_position, render::from_axis_angle({0, 0, 1}, root_yaw), {1, 1, 1}};
    frame_.door_handle = to_world(vehicle, rig_.handle);
    return true;
}

const EntryFrame& VehicleEntry::step(float dt, const Transform& vehicle) {
    if (!(dt > 0.0f) || !std::isfinite(dt)) throw std::invalid_argument("VehicleEntry::step: dt must be finite and positive");
    if (!started_) throw std::logic_error("VehicleEntry::step: begin() has not been called (or was refused)");
    const float vehicle_yaw = yaw_of(vehicle);
    EntryFrame& f = frame_;
    f.phase_time += dt;
    const Float3 stand_world = to_world(vehicle, rig_.stand_point);
    const float stand_heading = vehicle_yaw + rig_.stand_yaw;
    switch (f.phase) {
    case EntryPhase::Approach: {
        // Walk to the stand point (the nearest the character gets), turning toward the way it walks and, on arrival, to the standing heading.
        Float3 to = stand_world - f.root.position;
        to.z = 0.0f;
        const float d = length(to);
        float yaw = yaw_of(f.root);
        const float step_len = config_.walk_speed * dt;
        if (d > step_len) {
            const float want = std::atan2(to.y, to.x);
            const float turn = std::clamp(wrap_pi(want - yaw), -config_.turn_rate * dt, config_.turn_rate * dt);
            yaw += turn;
            const float aligned = std::max(0.0f, std::cos(wrap_pi(want - yaw)));
            f.root.position = f.root.position + Float3{std::cos(yaw), std::sin(yaw), 0.0f} * (step_len * aligned);
        } else {
            f.root.position = {stand_world.x, stand_world.y, f.root.position.z};
            const float turn = std::clamp(wrap_pi(stand_heading - yaw), -config_.turn_rate * dt, config_.turn_rate * dt);
            yaw += turn;
            if (std::fabs(wrap_pi(stand_heading - yaw)) < 1e-3f) {
                yaw = stand_heading;
                f.phase = EntryPhase::Reach;
                f.phase_time = 0.0f;
            }
        }
        f.root.rotation = render::from_axis_angle({0, 0, 1}, yaw);
        break;
    }
    case EntryPhase::Reach: {
        f.root.position = stand_world;
        f.root.rotation = render::from_axis_angle({0, 0, 1}, stand_heading);
        f.door_hand_weight = smooth(f.phase_time / config_.reach_time);
        if (f.phase_time >= config_.reach_time) {
            f.door_hand_weight = 1.0f;
            f.phase = EntryPhase::Open;
            f.phase_time = 0.0f;
        }
        break;
    }
    case EntryPhase::Open: {
        const float u = smooth(f.phase_time / config_.open_time);
        f.door_angle = rig_.door_max * u;
        f.root.position = to_world(vehicle, rig_.stand_point + rig_.open_step_back * u);
        f.root.rotation = render::from_axis_angle({0, 0, 1}, stand_heading);
        f.door_hand_weight = 1.0f;
        if (f.phase_time >= config_.open_time) {
            f.door_angle = rig_.door_max;
            f.phase = EntryPhase::Enter;
            f.phase_time = 0.0f;
            enter_from_ = rig_.stand_point + rig_.open_step_back;
            enter_yaw_from_ = rig_.stand_yaw;
        }
        break;
    }
    case EntryPhase::Enter: {
        const float u = std::clamp(f.phase_time / config_.enter_time, 0.0f, 1.0f);
        const float s = smooth(u);
        // Through the opening: from where the character stood, to the threshold, to the seat; arc length shared half and half.
        Float3 local;
        if (s < 0.5f) {
            local = enter_from_ + (rig_.threshold - enter_from_) * smooth(s * 2.0f);
        } else {
            local = rig_.threshold + (rig_.seat_root - rig_.threshold) * smooth((s - 0.5f) * 2.0f);
        }
        f.root.position = to_world(vehicle, local);
        const float yaw_local = lerp(enter_yaw_from_, rig_.seat_yaw, smooth(std::clamp(u * 1.5f, 0.0f, 1.0f)));
        f.root.rotation = render::from_axis_angle({0, 0, 1}, vehicle_yaw + yaw_local);
        f.hips_drop = (standing_hips_ - rig_.seat_hips_height) * smooth(std::clamp((u - 0.25f) / 0.75f, 0.0f, 1.0f));
        f.seat_weight = smooth(std::clamp((u - 0.4f) / 0.6f, 0.0f, 1.0f));
        f.door_hand_weight = 1.0f - smooth(std::clamp(u / 0.3f, 0.0f, 1.0f));
        f.wheel_weight = smooth(std::clamp((u - 0.45f) / 0.55f, 0.0f, 1.0f));
        if (f.phase_time >= config_.enter_time) {
            f.phase = EntryPhase::Seated;
            f.phase_time = 0.0f;
            f.hips_drop = standing_hips_ - rig_.seat_hips_height;
            f.seat_weight = 1.0f;
            f.wheel_weight = 1.0f;
            f.door_hand_weight = 0.0f;
        }
        break;
    }
    case EntryPhase::Seated:
        f.root.position = to_world(vehicle, rig_.seat_root);
        f.root.rotation = render::from_axis_angle({0, 0, 1}, vehicle_yaw + rig_.seat_yaw);
        f.finished = f.phase_time >= config_.settle_time;
        break;
    }
    // The door and what hangs on it, the wheel and the pedals follow the vehicle as it moves.
    const Quat swing = render::from_axis_angle(rig_.hinge_axis, f.door_angle);
    f.door_handle = to_world(vehicle, rig_.hinge_point + render::rotate(swing, rig_.handle - rig_.hinge_point));
    f.wheel_left = to_world(vehicle, rig_.wheel_left);
    f.wheel_right = to_world(vehicle, rig_.wheel_right);
    f.pedal_left = to_world(vehicle, rig_.pedal_left);
    f.pedal_right = to_world(vehicle, rig_.pedal_right);
    return frame_;
}

float apply_entry_pose(const gfx::Skin& skin, const HumanoidMap& map, const RigInfo& rig, gfx::Pose& pose, const EntryFrame& frame, bool door_hand_left, const ArmConstraints* arms) {
    const Transform& root = frame.root;
    const auto to_model = [&](Float3 world) { return render::rotate(render::conjugate(root.rotation), world - root.position); };
    const std::vector<JointWorld> standing = joint_world(skin, pose);
    // The hips come down: the whole upper body follows; the legs are solved below to keep the feet where they were (or take them to the pedals).
    const std::size_t hips = static_cast<std::size_t>(map[Bone::Hips]);
    if (frame.hips_drop != 0.0f) {
        const Float3 shift{0.0f, 0.0f, -frame.hips_drop};
        const JointWorld parent = parent_world(skin, standing, hips);
        Quat parent_rot = parent.rotation;
        if (skin.parent[hips] < 0) {
            const render::Mat4& b = skin.base[hips];
            const auto col = [&](int c) { return render::normalize(Float3{b.at(0, c), b.at(1, c), b.at(2, c)}); };
            parent_rot = render::from_basis(col(0), col(1), col(2));
        }
        pose.local[hips].position = pose.local[hips].position + render::rotate(render::conjugate(parent_rot), shift);
    }
    const struct {
        Bone upper, lower, foot, toes;
        bool left;
    } legs[2] = {{Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot, Bone::LeftToes, true}, {Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot, Bone::RightToes, false}};
    const Float3 knee_pole = Float3{1, 0, 0};
    for (const auto& leg : legs) {
        if (!map.has(leg.upper) || !map.has(leg.lower) || !map.has(leg.foot)) continue;
        const std::size_t ankle = static_cast<std::size_t>(map[leg.foot]);
        const std::size_t contact = static_cast<std::size_t>(map.has(leg.toes) ? map[leg.toes] : map[leg.foot]);
        const Float3 ankle_before = standing[ankle].position;
        const Float3 offset = standing[contact].position - standing[ankle].position;  // ankle to the contact point, in the standing foot's orientation
        const Float3 pedal = to_model(leg.left ? frame.pedal_left : frame.pedal_right);
        const Float3 target_contact = ankle_before + offset + (pedal - (ankle_before + offset)) * frame.seat_weight;
        // The foot's own height above its contact point stays: the contact sits on the target.
        (void)rig;
        solve_two_bone(skin, pose, static_cast<std::size_t>(map[leg.upper]), static_cast<std::size_t>(map[leg.lower]), ankle, target_contact - offset, knee_pole);
        // The foot keeps the orientation it had standing (flat), whatever the shin did.
        set_joint_world_rotation(skin, pose, joint_world(skin, pose), ankle, standing[ankle].rotation);
    }
    // The hands.
    float worst = 0.0f;
    const auto reach = [&](bool left, Float3 world_target, float weight) {
        if (!(weight > 0.0f)) return;
        (void)reach_hand(skin, map, rig, pose, root, left, world_target, weight, nullptr, arms);
        // The miss is measured against the wanted target, not the one the body volumes may have moved to: a handle inside the torso's reach is a miss.
        if (weight >= 0.999f) {
            const std::size_t hand = static_cast<std::size_t>(map[left ? Bone::LeftHand : Bone::RightHand]);
            const Float3 at = root.position + render::rotate(root.rotation, joint_world(skin, pose)[hand].position);
            worst = std::max(worst, length(at - world_target));
        }
    };
    reach(door_hand_left, frame.door_handle, frame.door_hand_weight);
    reach(true, frame.wheel_left, frame.wheel_weight);
    reach(false, frame.wheel_right, frame.wheel_weight);
    return worst;
}

} // namespace saige::anim
