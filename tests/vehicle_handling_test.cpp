// Vehicle handling options: the handbrake (rear brake force and a loss of rear grip: it stops the car and brings the tail round) and tire load sensitivity (a tire gives back less grip for the load a
// corner transfers onto it). Both are off by default and a configuration with them neutral is bit-identical to one without them.
#include "saige/vehicle.hpp"

#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

constexpr float kHuge = 5000.0f;
StaticCollision flat_ground() {
    std::vector<Aabb> boxes{{{-kHuge, -kHuge, -2}, {kHuge, kHuge, 0}}};
    return StaticCollision(boxes, {});
}

struct Rig {
    StaticCollision world = flat_ground();
    QueryScratch scratch;
};

void drive(Vehicle& v, const VehicleInput& in, double seconds, Rig& r) {
    const int ticks = static_cast<int>(std::lround(seconds * 60.0));
    for (int i = 0; i < ticks; ++i) v.step(in, 1.0f / 60.0f, r.world, r.scratch);
}

// A car at speed on the flat ground: accelerates (a straight run) and then coasts to the speed asked.
Vehicle at_speed(const VehicleConfig& cfg, double speed, Rig& r) {
    Vehicle v(cfg, {-240, 0, 0.9f}, 0.0f);  // start far back: the supported range is +-256 m about the origin
    VehicleInput gas;
    gas.throttle = 1.0f;
    for (int i = 0; i < 60 * 60 && v.state().forward_speed < speed; ++i) v.step(gas, 1.0f / 60.0f, r.world, r.scratch);
    return v;
}

float lateral_speed(const Vehicle& v) {
    const auto axes = v.axes();
    const Vec3& vel = v.state().velocity;
    return vel.x * axes[1].x + vel.y * axes[1].y + vel.z * axes[1].z;
}

bool same_state(const VehicleState& a, const VehicleState& b) {
    return std::memcmp(&a.position, &b.position, sizeof(Vec3)) == 0 && std::memcmp(&a.velocity, &b.velocity, sizeof(Vec3)) == 0 && std::memcmp(&a.orientation, &b.orientation, sizeof(VehicleQuat)) == 0 &&
           std::memcmp(&a.angular_velocity, &b.angular_velocity, sizeof(Vec3)) == 0;
}

void test_validation() {
    VehicleConfig c;
    const auto bad = [&](VehicleConfig x) {
        try {
            validate_vehicle_config(x);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    c.tire_load_sensitivity = 1.5f;
    require(bad(c), "load sensitivity above 1 is rejected");
    c = {};
    c.tire_load_sensitivity = -0.1f;
    require(bad(c), "negative load sensitivity is rejected");
    c = {};
    c.handbrake_grip = 0.0f;
    require(bad(c), "a handbrake that leaves no grip at all is rejected");
    c = {};
    c.handbrake_force = std::nanf("");
    require(bad(c), "a NaN handbrake force is rejected");
    c = {};
    c.tire_load_sensitivity = 0.2f;
    require(!bad(c), "a valid handling configuration is accepted");
    Rig r;
    Vehicle v(VehicleConfig{}, {0, 0, 0.9f}, 0.0f);
    VehicleInput in;
    in.handbrake = std::nanf("");
    bool threw = false;
    try { v.step(in, 1.0f / 60.0f, r.world, r.scratch); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "a non-finite handbrake input is rejected");
}

void test_neutral_handling_is_bit_identical() {
    Rig r;
    VehicleConfig plain;
    VehicleConfig neutral;
    neutral.tire_load_sensitivity = 0.0f;
    neutral.handbrake_force = 0.0f;
    neutral.handbrake_grip = 1.0f;
    Vehicle a(plain, {0, 0, 0.9f}, 0.0f), b(neutral, {0, 0, 0.9f}, 0.0f);
    bool same = true;
    for (int i = 0; i < 600; ++i) {
        VehicleInput in;
        in.throttle = (i % 200) < 150 ? 1.0f : 0.0f;
        in.brake = (i % 200) >= 150 ? 0.6f : 0.0f;
        in.steer = std::sin(static_cast<float>(i) * 0.02f);
        VehicleInput held = in;
        held.handbrake = (i % 300) > 220 ? 1.0f : 0.0f;  // pulled, but with no force and no grip loss configured it does nothing
        a.step(in, 1.0f / 60.0f, r.world, r.scratch);
        b.step(held, 1.0f / 60.0f, r.world, r.scratch);
        same = same && same_state(a.state(), b.state());
    }
    require(same, "a handbrake with no force and no grip loss, and no load sensitivity, leaves every state bit-identical");
}

void test_handbrake_stops_the_car_and_swings_the_tail() {
    Rig r;
    const VehicleConfig cfg;
    // Stopping from 15 m/s: coasting, handbrake alone, and the full brake.
    const auto stop_distance = [&](float brake, float handbrake) {
        Vehicle v = at_speed(cfg, 15.0, r);
        const float x0 = v.state().position.x;
        VehicleInput in;
        in.brake = brake;
        in.handbrake = handbrake;
        // Coasting takes hundreds of meters (the supported range is +-256 m): the distance is capped at 120 m, plenty to rank it above the handbrake.
        for (int i = 0; i < 60 * 40 && v.state().forward_speed > 0.2f && v.state().position.x - x0 < 120.0f; ++i) v.step(in, 1.0f / 60.0f, r.world, r.scratch);
        return v.state().position.x - x0;
    };
    const float coast = stop_distance(0.0f, 0.0f), hand = stop_distance(0.0f, 1.0f), full = stop_distance(1.0f, 0.0f);
    std::cout << "  stopping from 15 m/s: coasting " << coast << " m, handbrake " << hand << " m, full brake " << full << " m\n";
    require(coast > hand && hand > full, "the handbrake stops the car sooner than coasting but not as soon as the full brake");
    // In a corner: the same steady turn, then the handbrake is pulled; the tail comes round (rear sideslip) and the yaw rate rises.
    const auto corner = [&](float handbrake) {
        Vehicle v = at_speed(cfg, 16.0, r);
        VehicleInput turn;
        turn.steer = 0.35f;
        turn.throttle = 0.25f;
        drive(v, turn, 2.0, r);
        turn.handbrake = handbrake;
        double worst_beta = 0.0, worst_yaw = 0.0;
        for (int i = 0; i < 90; ++i) {
            v.step(turn, 1.0f / 60.0f, r.world, r.scratch);
            const double beta = std::atan2(static_cast<double>(std::fabs(lateral_speed(v))), std::max(1.0, static_cast<double>(std::fabs(v.state().forward_speed))));
            worst_beta = std::max(worst_beta, beta);
            worst_yaw = std::max(worst_yaw, static_cast<double>(std::fabs(v.state().angular_velocity.z)));
        }
        return std::pair<double, double>{worst_beta, worst_yaw};
    };
    const auto without = corner(0.0f), with = corner(1.0f);
    std::cout << "  corner at 16 m/s: body sideslip " << without.first * 57.2958 << " -> " << with.first * 57.2958 << " deg with the handbrake, yaw rate " << without.second << " -> " << with.second << " rad/s\n";
    require(with.first > 1.8 * without.first + 0.02, "the handbrake in a corner swings the tail: the body slides sideways much more");
    require(with.second > without.second, "and the yaw rate rises");
    // It does nothing to the steered wheels' own grip: pulled on a straight line the car does not spin.
    Vehicle straight = at_speed(cfg, 16.0, r);
    VehicleInput pull;
    pull.handbrake = 1.0f;
    drive(straight, pull, 1.0, r);
    require(std::fabs(straight.state().angular_velocity.z) < 0.2f && std::fabs(lateral_speed(straight)) < 0.5f, "pulled on a straight line the car stays straight");
    // A handbrake over 1 is 1.
    Vehicle x = at_speed(cfg, 12.0, r), y = at_speed(cfg, 12.0, r);
    VehicleInput one, two;
    one.handbrake = 1.0f;
    two.handbrake = 7.0f;
    drive(x, one, 1.0, r);
    drive(y, two, 1.0, r);
    require(same_state(x.state(), y.state()), "a handbrake input above 1 is clamped to 1");
}

void test_load_sensitivity_lowers_cornering_grip() {
    Rig r;
    const auto cornering = [&](float k) {
        VehicleConfig cfg;
        cfg.tire_load_sensitivity = k;
        Vehicle v = at_speed(cfg, 22.0, r);
        VehicleInput turn;
        turn.steer = 0.30f;
        turn.throttle = 0.35f;  // holds the speed against drag in the turn
        drive(v, turn, 2.0, r);
        double lateral_accel = 0.0;
        int n = 0;
        for (int i = 0; i < 60; ++i) {
            v.step(turn, 1.0f / 60.0f, r.world, r.scratch);
            lateral_accel += std::fabs(static_cast<double>(v.state().angular_velocity.z)) * static_cast<double>(std::max(1.0f, v.state().forward_speed));
            ++n;
        }
        return lateral_accel / n;
    };
    const double none = cornering(0.0f), some = cornering(0.30f), more = cornering(0.60f);
    std::cout << "  lateral acceleration in the same turn: k = 0 " << none << ", 0.3 " << some << ", 0.6 " << more << " m/s^2\n";
    require(some < none * 0.995 && more < some * 0.995, "more load sensitivity, less cornering grip");
    require(none > 5.0, "the reference turn really is at the limit of grip (several m/s^2)");
}

} // namespace

int main() {
    try {
        test_validation();
        test_neutral_handling_is_bit_identical();
        test_handbrake_stops_the_car_and_swings_the_tail();
        test_load_sensitivity_lowers_cornering_grip();
    } catch (const std::exception& e) {
        std::cerr << "vehicle handling test failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "vehicle handling tests passed\n";
}
