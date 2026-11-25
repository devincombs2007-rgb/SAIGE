#include "saige/vehicle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using saige::Aabb;
using saige::QueryScratch;
using saige::StaticCollision;
using saige::Triangle;
using saige::Vec3;
using saige::Vehicle;
using saige::VehicleConfig;
using saige::VehicleInput;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

template <class F>
bool throws_invalid(F&& f) {
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

const Aabb floor_box{{-5000, -5000, -2}, {5000, 5000, 0}};  // ground: top face at z = 0 (primitives may extend anywhere; the vehicle stays inside the +-256 m envelope)

StaticCollision flat_world() { return StaticCollision(std::vector<Aabb>{floor_box}, {}); }

// Total distance the world has been shifted by origin migration (the vehicle is brought back toward the origin every 100 m of travel along +X on an
// unbounded flat floor, where the shift changes nothing else).
double g_shift = 0.0;
void tick(Vehicle& v, const VehicleInput& in, const StaticCollision& world, QueryScratch& scratch) {
    v.step(in, 1.0f / 60.0f, world, scratch);
    if (v.state().position.x > 150.0f) {
        v.migrate_origin({-100.0f, 0.0f, 0.0f});
        g_shift += 100.0;
    }
}
void run(Vehicle& v, const VehicleInput& in, double seconds, const StaticCollision& world, QueryScratch& scratch) {
    const int ticks = static_cast<int>(std::lround(seconds * 60.0));
    for (int i = 0; i < ticks; ++i) tick(v, in, world, scratch);
}

double roll_pitch_max(const Vehicle& v) {
    const auto a = v.axes();
    // The up axis' tilt from vertical, and the forward axis' elevation.
    return std::max(std::acos(std::min(1.0, static_cast<double>(a[2].z))), std::fabs(std::asin(std::clamp(static_cast<double>(a[0].z), -1.0, 1.0))));
}

void test_validation() {
    const VehicleConfig base;
    saige::validate_vehicle_config(base);
    const auto bad = [&](auto mutate) {
        VehicleConfig c = base;
        mutate(c);
        return throws_invalid([&] { saige::validate_vehicle_config(c); });
    };
    const float nan = std::numeric_limits<float>::quiet_NaN();
    require(bad([](VehicleConfig& c) { c.mass = 0.0f; }), "zero mass");
    require(bad([&](VehicleConfig& c) { c.mass = nan; }), "NaN mass");
    require(bad([](VehicleConfig& c) { c.spring = -1.0f; }), "negative spring");
    require(bad([](VehicleConfig& c) { c.damper = 0.0f; }), "zero damper");
    require(bad([](VehicleConfig& c) { c.wheel_radius = 0.0f; }), "zero wheel radius");
    require(bad([](VehicleConfig& c) { c.tire_friction = 4.0f; }), "huge friction");
    require(bad([](VehicleConfig& c) { c.body_restitution = 1.0f; }), "restitution one");
    require(bad([](VehicleConfig& c) { c.half_extents.y = 0.0f; }), "flat chassis");
    require(bad([](VehicleConfig& c) { c.center_of_mass.z = 2.0f; }), "center of mass outside the box");
    require(bad([](VehicleConfig& c) { c.wheels[0].mount.x = 50.0f; }), "wheel mount far outside");
    require(bad([](VehicleConfig& c) { for (auto& w : c.wheels) w.driven = false; }), "no driven wheel");
    require(bad([](VehicleConfig& c) { c.max_steer = 1.6f; }), "extreme steering");
    require(throws_invalid([&] { Vehicle v(base, {0, 0, std::numeric_limits<float>::infinity()}, 0.0f); }), "infinite position");
    const StaticCollision world = flat_world();
    QueryScratch scratch;
    Vehicle v(base, {0, 0, 1.0f}, 0.0f);
    require(throws_invalid([&] { v.step({}, 0.0f, world, scratch); }), "zero dt");
    require(throws_invalid([&] { v.step({}, nan, world, scratch); }), "NaN dt");
    require(throws_invalid([&] { v.step({nan, 0, 0}, 0.016f, world, scratch); }), "NaN input");
    // Input is clamped, not refused.
    v.step({5.0f, 5.0f, -5.0f}, 0.016f, world, scratch);
}

void test_rest_and_ride_height() {
    const StaticCollision world = flat_world();
    QueryScratch scratch;
    Vehicle v({}, {0, 0, 1.0f}, 0.7f);
    run(v, {}, 4.0, world, scratch);
    const auto& s = v.state();
    // Static load: each wheel carries mass * g / 4 through its spring, so the compression is that over the spring rate and the box center sits
    // (mount drop + travel - compression + radius) above the ground.
    const VehicleConfig& c = v.config();
    const double load = static_cast<double>(c.mass) * 9.81 / 4.0;
    const double compression = load / static_cast<double>(c.spring);
    const double expected = -static_cast<double>(c.wheels[0].mount.z) + (static_cast<double>(c.suspension_travel) - compression) + static_cast<double>(c.wheel_radius);
    require(near(s.position.z, expected, 0.03), "rest height " + std::to_string(s.position.z) + " vs " + std::to_string(expected));
    require(s.wheels_in_contact == 4, "all four wheels carry the vehicle");
    for (const auto& w : s.wheels) require(near(w.compression, compression, 0.02), "each wheel compresses by its static share");
    require(std::fabs(s.forward_speed) < 0.05 && std::hypot(s.velocity.x, s.velocity.y) < 0.05, "at rest");
    require(roll_pitch_max(v) < 0.01, "level at rest");
    require(near(s.yaw, 0.7, 0.01), "heading kept");
}

void test_acceleration_and_braking() {
    const StaticCollision world = flat_world();
    QueryScratch scratch;
    Vehicle v({}, {0, 0, 1.0f}, 0.0f);
    run(v, {}, 2.0, world, scratch);
    g_shift = 0.0;
    const double start_x = static_cast<double>(v.state().position.x);
    double last = 0.0;
    for (int i = 0; i < 10; ++i) {
        run(v, {1.0f, 0, 0}, 1.0, world, scratch);
        const double speed = static_cast<double>(v.state().forward_speed);
        require(speed >= last - 0.05, "speed rises while the throttle is held");
        last = speed;
    }
    require(last > 15.0 && last < 60.0, "ten seconds of throttle reach a sane speed: " + std::to_string(last));
    require(std::fabs(static_cast<double>(v.state().position.y)) < 0.5, "tracks straight");
    require(static_cast<double>(v.state().position.x) + g_shift - start_x > 100.0, "covers ground");
    // Braking: from speed to a stop in a plausible distance and time.
    const double brake_start_x = static_cast<double>(v.state().position.x) + g_shift;
    const double brake_speed = static_cast<double>(v.state().forward_speed);
    double t = 0.0;
    while (static_cast<double>(v.state().forward_speed) > 0.3 && t < 12.0) {
        tick(v, {0.0f, 1.0f, 0}, world, scratch);
        t += 1.0 / 60.0;
    }
    const double distance = static_cast<double>(v.state().position.x) + g_shift - brake_start_x;
    const double decel = brake_speed * brake_speed / (2.0 * distance);
    require(t < 8.0, "brakes stop the car: " + std::to_string(t));
    require(decel > 0.4 * 9.81 && decel < 1.2 * 9.81, "braking deceleration is between 0.4 g and 1.2 g: " + std::to_string(decel / 9.81) + " g");
    // Reverse works and is slower.
    run(v, {}, 1.0, world, scratch);
    run(v, {-1.0f, 0, 0}, 4.0, world, scratch);
    require(v.state().forward_speed < -2.0 && v.state().forward_speed > -15.0, "reverse");
}

void test_steering_turns_on_the_geometry() {
    const StaticCollision world = flat_world();
    QueryScratch scratch;
    Vehicle v({}, {0, 0, 1.0f}, 0.0f);
    run(v, {}, 1.0, world, scratch);
    // Reach a steady 8 m/s, then hold a gentle steering angle.
    while (static_cast<double>(v.state().forward_speed) < 8.0) tick(v, {0.6f, 0, 0}, world, scratch);
    for (int i = 0; i < 180; ++i) tick(v, {0.12f, 0, 0.35f}, world, scratch);
    const double speed = static_cast<double>(v.state().forward_speed);
    const double yaw_rate = static_cast<double>(v.state().angular_velocity.z);
    const double wheelbase = 2.6;
    const double steer_angle = static_cast<double>(v.state().wheels[0].steer_angle);
    require(steer_angle > 0.05 && steer_angle < 0.25, "the front wheels are steered: " + std::to_string(steer_angle));
    const double expected_rate = speed * std::tan(steer_angle) / wheelbase;
    require(yaw_rate > 0.0 && near(yaw_rate, expected_rate, 0.3 * expected_rate), "yaw rate follows the bicycle model: " + std::to_string(yaw_rate) + " vs " + std::to_string(expected_rate));
    require(roll_pitch_max(v) < 0.12, "no wild body roll in a gentle turn");
    // Right steer turns the other way.
    Vehicle r({}, {0, 0, 1.0f}, 0.0f);
    run(r, {}, 1.0, world, scratch);
    while (static_cast<double>(r.state().forward_speed) < 8.0) r.step({0.6f, 0, 0}, 1.0f / 60.0f, world, scratch);
    for (int i = 0; i < 120; ++i) r.step({0.12f, 0, -0.35f}, 1.0f / 60.0f, world, scratch);
    require(r.state().angular_velocity.z < -0.05, "right steer");
}

void test_slope_and_pitch() {
    // A 12 degree ramp rising along +X from x = 10 to x = 40, a plateau after it.
    const double angle = 12.0 * 3.14159265358979 / 180.0;
    const float run_x = 30.0f, rise = static_cast<float>(30.0 * std::tan(angle));
    std::vector<Triangle> tris;
    const auto quad = [&](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
        tris.push_back({a, b, c});
        tris.push_back({a, c, d});
    };
    quad({10, -8, 0}, {10 + run_x, -8, rise}, {10 + run_x, 8, rise}, {10, 8, 0});
    quad({10 + run_x, -8, rise}, {120, -8, rise}, {120, 8, rise}, {10 + run_x, 8, rise});
    const StaticCollision world(std::vector<Aabb>{floor_box}, tris);
    QueryScratch scratch;
    Vehicle v({}, {0, 0, 1.0f}, 0.0f);
    run(v, {}, 1.5, world, scratch);
    double max_pitch = 0.0;
    bool on_plateau = false;
    for (int i = 0; i < 60 * 20 && !on_plateau; ++i) {
        v.step({1.0f, 0, 0}, 1.0f / 60.0f, world, scratch);
        if (v.state().position.x > 20.0f && v.state().position.x < 36.0f && v.state().wheels_in_contact == 4) {
            max_pitch = std::max(max_pitch, std::asin(static_cast<double>(v.axes()[0].z)));
        }
        on_plateau = v.state().position.x > 60.0f;
    }
    require(on_plateau, "the vehicle climbs a 12 degree ramp under power");
    require(near(max_pitch, angle, 0.06), "the body pitches with the slope: " + std::to_string(max_pitch) + " vs " + std::to_string(angle));
    require(static_cast<double>(v.state().position.z) > static_cast<double>(rise), "and ends up on the plateau");
}

void test_bump_damping() {
    // A kerb 0.12 m high crossed at 6 m/s: the body returns to its ride height and the oscillation dies.
    const StaticCollision world(std::vector<Aabb>{floor_box, Aabb{{20, -8, 0}, {20.4f, 8, 0.12f}}}, {});
    QueryScratch scratch;
    Vehicle v({}, {0, 0, 1.0f}, 0.0f);
    run(v, {}, 1.5, world, scratch);
    const double ride = static_cast<double>(v.state().position.z);
    while (static_cast<double>(v.state().forward_speed) < 6.0) v.step({0.5f, 0, 0}, 1.0f / 60.0f, world, scratch);
    double max_z = 0.0;
    while (v.state().position.x < 26.0f) {
        v.step({0.15f, 0, 0}, 1.0f / 60.0f, world, scratch);
        max_z = std::max(max_z, static_cast<double>(v.state().position.z));
    }
    require(max_z > ride + 0.01, "the kerb lifts the body");
    require(max_z < ride + 0.3, "but the suspension absorbs most of it");
    // Settle on the far side with the throttle off and the brake on.
    for (int i = 0; i < 60 * 6; ++i) v.step({0.0f, 1.0f, 0}, 1.0f / 60.0f, world, scratch);
    require(near(v.state().position.z, ride, 0.03), "the body returns to its ride height");
    require(roll_pitch_max(v) < 0.02, "and level");
}

void test_wall_impact() {
    const StaticCollision world(std::vector<Aabb>{floor_box, Aabb{{30, -6, 0}, {30.6f, 6, 1.2f}}}, {});
    QueryScratch scratch;
    Vehicle v({}, {-20.0f, 0.0f, 1.0f}, 0.0f);
    run(v, {}, 1.0, world, scratch);
    while (static_cast<double>(v.state().forward_speed) < 19.5 && v.state().position.x < 24.0f) v.step({1.0f, 0, 0}, 1.0f / 60.0f, world, scratch);
    const double speed_in = static_cast<double>(v.state().forward_speed);
    const double ke_in = v.kinetic_energy();
    std::vector<saige::VehicleImpact> events;
    double penetration = 0.0, rebound = 0.0;
    double ke_settled = 0.0;
    for (int i = 0; i < 60 * 3; ++i) {
        v.step({0.0f, i < 60 ? 0.0f : 1.0f, 0}, 1.0f / 60.0f, world, scratch);  // free for a second after the hit, then the brake
        for (const auto& e : v.take_impacts()) events.push_back(e);
        penetration = std::max(penetration, static_cast<double>(v.state().position.x) + 2.15 - 30.0);
        rebound = std::min(rebound, static_cast<double>(v.state().forward_speed));
        if (i == 59) ke_settled = v.kinetic_energy();
    }
    require(speed_in > 19.0, "set up at speed");
    require(!events.empty(), "the crash is reported");
    double max_event_speed = 0.0;
    for (const auto& e : events) max_event_speed = std::max(max_event_speed, static_cast<double>(e.speed));
    require(max_event_speed > 12.0 && max_event_speed < 25.0, "the reported approach speed matches the driving speed: " + std::to_string(max_event_speed));
    require(events.front().normal.x < -0.7, "the impact normal points back along the road, from the wall toward the car");
    require(penetration < 0.3, "the car does not tunnel into the barrier: " + std::to_string(penetration));
    require(std::fabs(static_cast<double>(v.state().forward_speed)) < 1.0, "it comes to rest against the barrier");
    require(rebound > -6.0, "the rebound is small: " + std::to_string(rebound));
    require(static_cast<double>(v.state().position.x) < 30.0, "it stays on its side of the barrier");
    require(ke_settled < ke_in * 0.02, "the collision dissipates almost all the kinetic energy within a second: " + std::to_string(ke_settled / ke_in));
}

void test_off_centre_impact_spins_the_car() {
    // The same wall, a head-on hit and one with the car's nose 0.9 m off the wall's centre line is not different geometry; to make it off-centre the wall ends at y = 0.
    const StaticCollision world(std::vector<Aabb>{floor_box, Aabb{{30, 0.2f, 0}, {30.6f, 6, 1.2f}}}, {});
    QueryScratch scratch;
    Vehicle v({}, {0.0f, 0.0f, 1.0f}, 0.0f);
    run(v, {}, 1.0, world, scratch);
    while (static_cast<double>(v.state().forward_speed) < 14.5 && v.state().position.x < 24.0f) v.step({1.0f, 0, 0}, 1.0f / 60.0f, world, scratch);
    double yaw_max = 0.0, yaw_travel = 0.0;
    const double yaw0 = static_cast<double>(v.state().yaw);
    for (int i = 0; i < 60 * 3; ++i) {
        v.step({0.0f, 0.0f, 0}, 1.0f / 60.0f, world, scratch);
        yaw_max = std::max(yaw_max, std::fabs(static_cast<double>(v.state().angular_velocity.z)));
        yaw_travel = std::max(yaw_travel, std::fabs(static_cast<double>(v.state().yaw) - yaw0));
    }
    require(yaw_max > 0.5, "a glancing hit makes the car spin: peak yaw rate " + std::to_string(yaw_max));
    require(yaw_travel > 0.15, "and turn: " + std::to_string(yaw_travel));
    // A head-on hit on the centre line barely yaws.
    const StaticCollision full(std::vector<Aabb>{floor_box, Aabb{{30, -6, 0}, {30.6f, 6, 1.2f}}}, {});
    Vehicle c({}, {0.0f, 0.0f, 1.0f}, 0.0f);
    run(c, {}, 1.0, full, scratch);
    while (static_cast<double>(c.state().forward_speed) < 14.5 && c.state().position.x < 24.0f) c.step({1.0f, 0, 0}, 1.0f / 60.0f, full, scratch);
    double centred = 0.0;
    for (int i = 0; i < 60 * 3; ++i) {
        c.step({0.0f, 0.0f, 0}, 1.0f / 60.0f, full, scratch);
        centred = std::max(centred, std::fabs(static_cast<double>(c.state().angular_velocity.z)));
    }
    require(centred < yaw_max * 0.5, "a centred hit spins far less: " + std::to_string(centred) + " vs " + std::to_string(yaw_max));
}

void test_no_tunnelling_at_speed() {
    // A thin wall (0.3 m) after a 150 m run-up at full throttle: the car arrives above 25 m/s, moving 0.2 m per 1/120 s substep.
    const StaticCollision world(std::vector<Aabb>{floor_box, Aabb{{150, -8, 0}, {150.3f, 8, 1.5f}}}, {});
    QueryScratch scratch;
    Vehicle v({}, {0.0f, 0.0f, 1.0f}, 0.0f);
    run(v, {}, 1.0, world, scratch);
    double arrival = 0.0;
    for (int i = 0; i < 60 * 20 && static_cast<double>(v.state().position.x) < 150.3; ++i) {
        v.step({1.0f, 0, 0}, 1.0f / 60.0f, world, scratch);
        arrival = std::max(arrival, static_cast<double>(v.state().forward_speed));
        require(static_cast<double>(v.state().position.x) < 150.4, "the chassis never passes through the thin wall");
    }
    require(arrival > 25.0, "arrived fast: " + std::to_string(arrival));
    for (int i = 0; i < 60 * 2; ++i) {
        v.step({1.0f, 0, 0}, 1.0f / 60.0f, world, scratch);
        require(static_cast<double>(v.state().position.x) < 150.4, "and stays on its side");
    }
}

void test_determinism_and_origin_migration() {
    const StaticCollision world(std::vector<Aabb>{floor_box, Aabb{{30, -6, 0}, {30.6f, 6, 1.2f}}}, {});
    QueryScratch s1, s2;
    Vehicle a({}, {5.0f, 0.0f, 1.0f}, 0.2f), b({}, {5.0f, 0.0f, 1.0f}, 0.2f);
    for (int i = 0; i < 60 * 8; ++i) {
        const VehicleInput in{i < 300 ? 1.0f : 0.0f, i > 360 ? 0.5f : 0.0f, i % 90 < 45 ? 0.3f : -0.3f};
        a.step(in, 1.0f / 60.0f, world, s1);
        b.step(in, 1.0f / 60.0f, world, s2);
    }
    require(a.state().position.x == b.state().position.x && a.state().position.y == b.state().position.y && a.state().position.z == b.state().position.z &&
                a.state().orientation.w == b.state().orientation.w && a.state().velocity.x == b.state().velocity.x,
            "two vehicles driven alike are identical");
    // Migrating the origin shifts the body and nothing else.
    const auto before = a.state();
    a.migrate_origin({-64.0f, 0.0f, 0.0f});
    require(near(a.state().position.x, static_cast<double>(before.position.x) - 64.0, 1e-3) && a.state().velocity.x == before.velocity.x && a.state().orientation.z == before.orientation.z,
            "origin migration moves the body and keeps its motion");
}

void test_rollover_is_possible_but_not_easy() {
    // Hard steering at speed on flat ground: the car slides, it does not flip (the center of mass is low).
    const StaticCollision world = flat_world();
    QueryScratch scratch;
    Vehicle v({}, {0, 0, 1.0f}, 0.0f);
    run(v, {}, 1.0, world, scratch);
    while (static_cast<double>(v.state().forward_speed) < 22.0) v.step({1.0f, 0, 0}, 1.0f / 60.0f, world, scratch);
    double worst = 0.0;
    for (int i = 0; i < 60 * 4; ++i) {
        v.step({0.0f, 0.0f, 1.0f}, 1.0f / 60.0f, world, scratch);
        worst = std::max(worst, std::acos(std::min(1.0, static_cast<double>(v.axes()[2].z))));
    }
    require(worst < 0.6, "a full-lock turn at 22 m/s skids and rolls the body less than 35 degrees: " + std::to_string(worst));
}

} // namespace

int main() {
    try {
        test_validation();
        test_rest_and_ride_height();
        test_acceleration_and_braking();
        test_steering_turns_on_the_geometry();
        test_slope_and_pitch();
        test_bump_damping();
        test_wall_impact();
        test_off_centre_impact_spins_the_car();
        test_no_tunnelling_at_speed();
        test_determinism_and_origin_migration();
        test_rollover_is_possible_but_not_easy();
    } catch (const std::exception& error) {
        std::cerr << "vehicle test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "vehicle tests passed\n";
}
