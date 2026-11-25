// Getting into a vehicle: the reference sequence on a synthetic humanoid (approach the car, reach the door handle, open the door, step in, sit with the hands on the wheel and the feet on the pedals).
#include "saige/gfx_test/saige_test.hpp"

#include "saige/animation/seat_entry.hpp"

#include "fixtures.hpp"

#include <cstdio>

using namespace saige;
using namespace saige::anim;
using render::Float3;

namespace {

float dist(Float3 a, Float3 b) { return render::length(a - b); }

// A left-hand-drive car facing +X: the driver's door on its left (+Y) side, hinged at the front of the door, handle behind the hinge; the character walks up to it from outside and uses its right hand.
VehicleEntryRig car_rig() {
    VehicleEntryRig r;
    r.hinge_point = {0.6f, 0.95f, 0.6f};
    r.hinge_axis = {0.0f, 0.0f, -1.0f};  // clockwise from above: the handle behind the hinge swings out to +Y
    r.handle = {-0.1f, 0.98f, 1.15f};
    r.door_max = 1.1f;
    r.door_hand_left = false;
    r.stand_point = {0.0f, 1.45f, 0.0f};
    r.stand_yaw = 0.0f;
    r.open_step_back = {0.0f, 0.3f, 0.0f};
    r.threshold = {0.25f, 0.9f, 0.0f};
    r.seat_root = {0.0f, 0.45f, 0.0f};
    r.seat_hips_height = 0.5f;
    r.seat_yaw = 0.0f;
    r.wheel_left = {0.38f, 0.6f, 0.95f};
    r.wheel_right = {0.38f, 0.3f, 0.95f};
    r.pedal_left = {0.7f, 0.5f, 0.1f};
    r.pedal_right = {0.7f, 0.4f, 0.1f};
    return r;
}

struct Run {
    std::vector<EntryPhase> order;
    float total = 0.0f;
    bool finished = false;
};

render::Transform vehicle_at(Float3 p, float yaw) { return render::Transform{p, render::from_axis_angle({0, 0, 1}, yaw), {1, 1, 1}}; }

} // namespace

TEST_CASE("Vehicle entry: the phases come in order and end seated at the hips, the wheel and the pedals the rig names") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::Pose rest = gfx::rest_pose(b.skin);
    const VehicleEntryRig rig = car_rig();
    VehicleEntry entry(rig);
    const render::Transform car = vehicle_at({12.0f, -7.0f, 0.0f}, 0.6f);
    const float standing_hips = joint_world(b.skin, rest)[static_cast<std::size_t>(b.map[Bone::Hips])].position.z;
    // Starting 5 m away, facing the wrong way.
    const Float3 start = car.position + render::rotate(car.rotation, Float3{-4.0f, 4.0f, 0.0f});
    REQUIRE(entry.begin(car, start, 2.5f, standing_hips, 0.0f));
    const ArmConstraints arms = [&] {
        ArmConstraints c;
        c.body = default_body_capsules(b.skin, b.map, rest);
        return c;
    }();
    std::vector<EntryPhase> order{EntryPhase::Approach};
    float t = 0.0f, worst_reach = 0.0f, worst_open = 0.0f, max_step = 0.0f, last_door = -1.0f, min_threshold = 1e9f, max_pose_jump = 0.0f, last_hips_drop = 0.0f;
    bool door_monotone = true, drop_monotone = true, radius_constant = true;
    Float3 last_root = start;
    gfx::Pose last_pose = rest;
    const Float3 hinge_world = car.position + render::rotate(car.rotation, rig.hinge_point);
    const float radius0 = dist(Float3{hinge_world.x, hinge_world.y, 0.0f}, Float3{(car.position + render::rotate(car.rotation, rig.handle)).x, (car.position + render::rotate(car.rotation, rig.handle)).y, 0.0f});
    EntryFrame last_frame;
    while (!entry.frame().finished && t < 30.0f) {
        const EntryFrame& f = entry.step(1.0f / 60.0f, car);
        t += 1.0f / 60.0f;
        if (order.back() != f.phase) order.push_back(f.phase);
        gfx::Pose pose = rest;
        const float miss = apply_entry_pose(b.skin, b.map, b.rig, pose, f, rig.door_hand_left, &arms);
        max_pose_jump = std::max(max_pose_jump, pose_distance(pose, last_pose));
        last_pose = pose;
        max_step = std::max(max_step, dist(f.root.position, last_root));
        last_root = f.root.position;
        // Measured from the pose itself, not from what the solver reports: where the door hand really is against the handle.
        const std::size_t door_hand = static_cast<std::size_t>(b.map[rig.door_hand_left ? Bone::LeftHand : Bone::RightHand]);
        const float hand_to_handle = dist(f.root.position + render::rotate(f.root.rotation, joint_world(b.skin, pose)[door_hand].position), f.door_handle);
        if (f.phase == EntryPhase::Reach && f.door_hand_weight >= 0.999f) worst_reach = std::max(worst_reach, std::max(miss, hand_to_handle));
        if (f.phase == EntryPhase::Open) {
            worst_open = std::max(worst_open, std::max(miss, hand_to_handle));
            door_monotone = door_monotone && f.door_angle >= last_door - 1e-6f;
            const Float3 h = f.door_handle;
            radius_constant = radius_constant && std::fabs(dist(Float3{hinge_world.x, hinge_world.y, 0.0f}, Float3{h.x, h.y, 0.0f}) - radius0) < 1e-4f;
            last_door = f.door_angle;
        }
        if (f.phase == EntryPhase::Enter) {
            min_threshold = std::min(min_threshold, dist(f.root.position, car.position + render::rotate(car.rotation, rig.threshold)));
            drop_monotone = drop_monotone && f.hips_drop >= last_hips_drop - 1e-6f;
            last_hips_drop = f.hips_drop;
        }
        last_frame = f;
    }
    std::printf("entry took %.2f s; phases:", static_cast<double>(t));
    for (const EntryPhase p : order) std::printf(" %d", static_cast<int>(p));
    std::printf("\nreach: hand %.1f mm from the handle; opening: worst %.1f mm; enter: closest to the threshold %.1f mm, largest root step %.1f mm per tick; largest pose change per tick %.3f rad\n", static_cast<double>(worst_reach) * 1000.0,
                static_cast<double>(worst_open) * 1000.0, static_cast<double>(min_threshold) * 1000.0, static_cast<double>(max_step) * 1000.0, static_cast<double>(max_pose_jump));
    CHECK(entry.frame().finished);
    REQUIRE(order.size() == 5);
    CHECK(order[0] == EntryPhase::Approach);
    CHECK(order[1] == EntryPhase::Reach);
    CHECK(order[2] == EntryPhase::Open);
    CHECK(order[3] == EntryPhase::Enter);
    CHECK(order[4] == EntryPhase::Seated);
    // Reaching and opening: the hand is on the handle within a centimeter and keeps up with it as the door swings (within two).
    CHECK(worst_reach < 0.01f);
    CHECK(worst_open < 0.02f);
    CHECK(door_monotone);
    CHECK(radius_constant);
    CHECK(std::fabs(last_door - rig.door_max) < 1e-4f);
    // Entering: through the opening, smoothly (3 m/s at the very most), the hips only going down.
    CHECK(min_threshold < 0.03f);
    CHECK(max_step < 0.05f);
    CHECK(drop_monotone);
    CHECK(max_pose_jump < 0.2f);
    // Seated.
    const EntryFrame& f = last_frame;
    gfx::Pose seated = rest;
    const float miss = apply_entry_pose(b.skin, b.map, b.rig, seated, f, rig.door_hand_left, &arms);
    const std::vector<JointWorld> w = joint_world(b.skin, seated);
    const auto world_of = [&](Float3 model) { return f.root.position + render::rotate(f.root.rotation, model); };
    const Float3 hips = world_of(w[static_cast<std::size_t>(b.map[Bone::Hips])].position);
    const Float3 want_hips = car.position + render::rotate(car.rotation, rig.seat_root + Float3{0, 0, rig.seat_hips_height});
    std::printf("seated: hips %.1f mm from the seat target, hands missed by %.1f mm\n", static_cast<double>(dist(hips, want_hips)) * 1000.0, static_cast<double>(miss) * 1000.0);
    CHECK(dist(hips, want_hips) < 0.02f);
    CHECK(miss < 0.01f);
    CHECK(dist(world_of(w[static_cast<std::size_t>(b.map[Bone::LeftHand])].position), f.wheel_left) < 0.01f);
    CHECK(dist(world_of(w[static_cast<std::size_t>(b.map[Bone::RightHand])].position), f.wheel_right) < 0.01f);
    for (const bool left : {true, false}) {
        const std::size_t contact = static_cast<std::size_t>(b.map.has(left ? Bone::LeftToes : Bone::RightToes) ? b.map[left ? Bone::LeftToes : Bone::RightToes] : b.map[left ? Bone::LeftFoot : Bone::RightFoot]);
        const std::size_t ankle = static_cast<std::size_t>(b.map[left ? Bone::LeftFoot : Bone::RightFoot]);
        CHECK(dist(world_of(w[contact].position), left ? f.pedal_left : f.pedal_right) < 0.02f);
        (void)ankle;
    }
    CHECK(rotation_angle_between(f.root.rotation, render::from_axis_angle({0, 0, 1}, 0.6f + rig.seat_yaw)) < 0.02f);
    // Bone lengths survive the whole thing (IK only turns joints).
    const std::vector<JointWorld> w0 = joint_world(b.skin, rest);
    for (const Bone bone : {Bone::LeftLowerArm, Bone::RightLowerArm, Bone::LeftLowerLeg, Bone::RightLowerLeg, Bone::LeftHand, Bone::RightHand}) {
        const std::size_t j = static_cast<std::size_t>(b.map[bone]);
        const std::size_t p = static_cast<std::size_t>(b.skin.parent[j]);
        CHECK(std::fabs(dist(w[j].position, w[p].position) - dist(w0[j].position, w0[p].position)) < 1e-3f);
    }
}

TEST_CASE("Vehicle entry: a slowly rolling vehicle is followed, a moving one is refused, and bad input is rejected") {
    const fixtures::Biped b = fixtures::make_biped();
    const gfx::Pose rest = gfx::rest_pose(b.skin);
    const VehicleEntryRig rig = car_rig();
    const float standing_hips = joint_world(b.skin, rest)[static_cast<std::size_t>(b.map[Bone::Hips])].position.z;
    VehicleEntry moving(rig);
    render::Transform car = vehicle_at({0.0f, 0.0f, 0.0f}, 0.0f);
    REQUIRE(!moving.begin(car, {-3, 3, 0}, 0.0f, standing_hips, 0.9f));  // too fast: refused, nothing started
    CHECK(!moving.active());
    bool threw = false;
    try { (void)moving.step(1.0f / 60.0f, car); } catch (const std::logic_error&) { threw = true; }
    CHECK(threw);
    REQUIRE(moving.begin(car, {-3, 3, 0}, 0.0f, standing_hips, 0.2f));
    // The car creeps forward at 0.2 m/s the whole time: the hand stays on the handle (which moves with the door and the car), the seat is where the car's seat is.
    ArmConstraints arms;
    arms.body = default_body_capsules(b.skin, b.map, rest);
    float worst = 0.0f;
    float t = 0.0f;
    EntryFrame last;
    while (!moving.frame().finished && t < 30.0f) {
        car.position.x += 0.2f / 60.0f;
        last = moving.step(1.0f / 60.0f, car);
        t += 1.0f / 60.0f;
        gfx::Pose pose = rest;
        const float miss = apply_entry_pose(b.skin, b.map, b.rig, pose, last, rig.door_hand_left, &arms);
        if (last.phase == EntryPhase::Open || last.phase == EntryPhase::Seated) worst = std::max(worst, miss);
    }
    CHECK(moving.frame().finished);
    CHECK(worst < 0.02f);
    const Float3 seat_world = car.position + render::rotate(car.rotation, rig.seat_root);
    CHECK(dist(last.root.position, seat_world) < 1e-4f);
    // Bad input.
    VehicleEntryConfig bad;
    bad.reach_time = 0.0f;
    threw = false;
    try { VehicleEntry e(rig, bad); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    VehicleEntryRig zero_axis = rig;
    zero_axis.hinge_axis = {0, 0, 0};
    threw = false;
    try { VehicleEntry e(zero_axis); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
    VehicleEntry ok(rig);
    CHECK(!ok.begin(car, {std::nanf(""), 0, 0}, 0.0f, standing_hips, 0.0f));
    REQUIRE(ok.begin(car, {-3, 3, 0}, 0.0f, standing_hips, 0.0f));
    threw = false;
    try { (void)ok.step(0.0f, car); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}
