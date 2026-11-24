// Physical surfaces under the vehicle: the material table, the surface map, bit-for-bit compatibility with no map, and what a tire does on different ground (stopping distance follows grip,
// braking on split friction yaws toward the grippy side, and full throttle gets less of a car moving on ice than on asphalt).
#include "saige/physics_material.hpp"
#include "saige/vehicle.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

constexpr float kHuge = 5000.0f;

// Two ground slabs side by side along `axis` (0: split at x = 0, so slab 0 is behind and slab 1 ahead; 1: split at y = 0, slab 0 is the left half (y > 0), slab 1 the right).
struct Ground {
    StaticCollision world;
    SurfaceMap surfaces;
};
Ground two_slabs(int axis, Surface first, Surface second) {
    std::vector<Aabb> boxes;
    if (axis == 0) {
        boxes.push_back({{-kHuge, -kHuge, -2}, {0, kHuge, 0}});
        boxes.push_back({{0, -kHuge, -2}, {kHuge, kHuge, 0}});
    } else {
        boxes.push_back({{-kHuge, 0, -2}, {kHuge, kHuge, 0}});
        boxes.push_back({{-kHuge, -kHuge, -2}, {kHuge, 0, 0}});
    }
    Ground g{StaticCollision(boxes, {}), {}};
    g.surfaces = SurfaceMap(g.world);
    g.surfaces.set({StaticPrimitive::Kind::Box, 0}, first);
    g.surfaces.set({StaticPrimitive::Kind::Box, 1}, second);
    return g;
}

void drive(Vehicle& v, const VehicleInput& in, double seconds, const Ground& g, const SurfaceMap* surfaces, QueryScratch& scratch) {
    const int ticks = static_cast<int>(std::lround(seconds * 60.0));
    for (int i = 0; i < ticks; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, surfaces);
}

bool same_state(const VehicleState& a, const VehicleState& b) {
    return std::memcmp(&a.position, &b.position, sizeof(Vec3)) == 0 && std::memcmp(&a.velocity, &b.velocity, sizeof(Vec3)) == 0 && std::memcmp(&a.orientation, &b.orientation, sizeof(VehicleQuat)) == 0 &&
           std::memcmp(&a.angular_velocity, &b.angular_velocity, sizeof(Vec3)) == 0;
}

void test_table_and_map() {
    std::set<std::string> names;
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(Surface::Count); ++i) {
        const PhysicsMaterial& m = physics_material(static_cast<Surface>(i));
        require(m.name != nullptr && m.name[0] != '\0', "every surface has a name");
        require(names.insert(m.name).second, std::string("surface names are unique: ") + m.name);
        require(m.tire_grip > 0.0f && m.tire_grip <= 1.0f, std::string("grip in (0, 1]: ") + m.name);
        require(m.rolling_resistance >= 1.0f, std::string("rolling resistance is at least dry asphalt's: ") + m.name);
        require(m.restitution >= 0.0f && m.restitution <= 1.0f && m.friction >= 0.0f && m.density > 0.0f, std::string("material values in range: ") + m.name);
    }
    require(physics_material(Surface::Default).tire_grip == 1.0f && physics_material(Surface::Default).rolling_resistance == 1.0f, "the default surface changes nothing");
    require(physics_material(Surface::Asphalt).tire_grip == 1.0f, "asphalt is the grip reference");
    require(physics_material(Surface::Ice).tire_grip < physics_material(Surface::Snow).tire_grip && physics_material(Surface::Snow).tire_grip < physics_material(Surface::Sand).tire_grip &&
                physics_material(Surface::Sand).tire_grip < physics_material(Surface::Grass).tire_grip && physics_material(Surface::Grass).tire_grip < physics_material(Surface::Gravel).tire_grip &&
                physics_material(Surface::Gravel).tire_grip < physics_material(Surface::AsphaltWet).tire_grip && physics_material(Surface::AsphaltWet).tire_grip < physics_material(Surface::Asphalt).tire_grip,
            "grip falls from dry asphalt to ice in the expected order");
    require(physics_material(static_cast<Surface>(250)).tire_grip == 1.0f, "an out-of-range surface reads as Default rather than outside the table");
    require(combine_friction(0.2f, 0.8f) == combine_friction(0.8f, 0.2f) && std::fabs(combine_friction(0.5f, 0.5f) - 0.5f) < 1e-6f && combine_friction(0.05f, 0.9f) < 0.25f, "friction combination is symmetric, idempotent and slippery-dominated");
    require(combine_restitution(0.1f, 0.6f) == 0.6f && combine_restitution(0.6f, 0.1f) == 0.6f, "restitution combines by the larger");

    SurfaceMap m(2, 3, Surface::Grass);
    require(m.at({StaticPrimitive::Kind::Box, 1}) == Surface::Grass && m.at({StaticPrimitive::Kind::Triangle, 2}) == Surface::Grass, "fill applies to every primitive");
    m.set({StaticPrimitive::Kind::Triangle, 2}, Surface::Ice);
    require(m.at({StaticPrimitive::Kind::Triangle, 2}) == Surface::Ice && m.at({StaticPrimitive::Kind::Box, 1}) == Surface::Grass, "set changes one primitive");
    require(m.at({StaticPrimitive::Kind::Box, 99}) == Surface::Default, "a primitive outside the map reads as Default");
    bool threw = false;
    try { m.set({StaticPrimitive::Kind::Box, 2}, Surface::Sand); } catch (const std::out_of_range&) { threw = true; }
    require(threw, "setting a primitive outside the map throws");
}

void test_no_map_is_the_old_behavior() {
    const Ground g = two_slabs(0, Surface::Asphalt, Surface::Asphalt);
    QueryScratch scratch;
    const VehicleInput in{1.0f, 0.0f, 0.2f};
    Vehicle none({}, {-100, 0, 1.0f}, 0.0f), def({}, {-100, 0, 1.0f}, 0.0f), asphalt({}, {-100, 0, 1.0f}, 0.0f);
    SurfaceMap all_default(g.world, Surface::Default), all_asphalt(g.world, Surface::Asphalt);
    drive(none, in, 6.0, g, nullptr, scratch);
    drive(def, in, 6.0, g, &all_default, scratch);
    drive(asphalt, in, 6.0, g, &all_asphalt, scratch);
    require(same_state(none.state(), def.state()), "no map and an all-Default map give the identical state");
    require(same_state(none.state(), asphalt.state()), "an all-Asphalt map (grip 1, rolling 1) gives the identical state");
}

// Speed 15 m/s on dry asphalt, then braking on `surface`: the distance to a stop.
struct Braking { double speed_at_brake, distance; };
Braking braking_on(Surface surface) {
    const Ground g = two_slabs(0, Surface::Asphalt, surface);
    QueryScratch scratch;
    Vehicle v({}, {-100, 0, 1.0f}, 0.0f);
    VehicleInput in{1.0f, 0.0f, 0.0f};
    for (int i = 0; i < 60 * 12 && v.state().forward_speed < 15.0f; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    in = {0.0f, 0.0f, 0.0f};  // coast onto the test surface
    for (int i = 0; i < 60 * 20 && v.state().position.x < 0.0f; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    const double v0 = static_cast<double>(v.state().forward_speed), x0 = static_cast<double>(v.state().position.x);
    in = {0.0f, 1.0f, 0.0f};
    for (int i = 0; i < 60 * 30 && v.state().forward_speed > 0.05f; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    return {v0, static_cast<double>(v.state().position.x) - x0};
}

void test_stopping_distance_follows_grip() {
    const VehicleConfig cfg;
    const Surface order[] = {Surface::Asphalt, Surface::AsphaltWet, Surface::Gravel, Surface::Grass, Surface::Sand, Surface::Snow, Surface::Ice};
    double previous = 0.0;
    for (const Surface s : order) {
        const Braking b = braking_on(s);
        const double grip = static_cast<double>(physics_material(s).tire_grip);
        std::cout << "  braking from " << b.speed_at_brake << " m/s on " << physics_material(s).name << ": " << b.distance << " m\n";
        require(b.distance > previous, std::string("stopping distance grows as grip falls (") + physics_material(s).name + ")");
        previous = b.distance;
        // Where friction, not the brake, limits the stop, d is v^2 / (2 mu g) for the tire's mu times the surface grip, plus a little for the soft sign at low speed and the weight transfer.
        const double mu = static_cast<double>(cfg.tire_friction) * grip;
        if (grip <= 0.7) {
            const double ideal = b.speed_at_brake * b.speed_at_brake / (2.0 * mu * 9.81);
            require(b.distance > 0.85 * ideal && b.distance < 1.30 * ideal, std::string("stopping distance is about v^2 / (2 mu g) on ") + physics_material(s).name + ": " + std::to_string(b.distance) + " vs " + std::to_string(ideal));
        }
    }
}

// Dry asphalt behind x = 0 for the run-up; ahead of it the ground is split along y = 0 into a left half (y > 0) and a right half.
Ground split_ahead(Surface left, Surface right) {
    std::vector<Aabb> boxes{{{-kHuge, -kHuge, -2}, {0, kHuge, 0}}, {{0, 0, -2}, {kHuge, kHuge, 0}}, {{0, -kHuge, -2}, {kHuge, 0, 0}}};
    Ground g{StaticCollision(boxes, {}), {}};
    g.surfaces = SurfaceMap(g.world);
    g.surfaces.set({StaticPrimitive::Kind::Box, 0}, Surface::Asphalt);
    g.surfaces.set({StaticPrimitive::Kind::Box, 1}, left);
    g.surfaces.set({StaticPrimitive::Kind::Box, 2}, right);
    return g;
}

// Degrees of heading gained over three seconds of braking from 15 m/s on a split-friction surface, driving straight onto it (positive: turned left).
double heading_change_braking_on_split(Surface left, Surface right) {
    const Ground g = split_ahead(left, right);
    QueryScratch scratch;
    Vehicle v({}, {-100, 0, 1.0f}, 0.0f);
    VehicleInput in{1.0f, 0.0f, 0.0f};
    for (int i = 0; i < 60 * 12 && v.state().forward_speed < 15.0f; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    in = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 60 * 20 && v.state().position.x < 0.0f; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    const double yaw0 = static_cast<double>(v.state().yaw);
    in = {0.0f, 1.0f, 0.0f};
    for (int i = 0; i < 60 * 3; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    double d = static_cast<double>(v.state().yaw) - yaw0;
    while (d > 3.14159265358979) d -= 6.28318530717958;
    while (d < -3.14159265358979) d += 6.28318530717958;
    return d * 180.0 / 3.14159265358979;
}

void test_split_friction_yaws_toward_the_grippy_side() {
    const double control = heading_change_braking_on_split(Surface::Asphalt, Surface::Asphalt);
    const double left = heading_change_braking_on_split(Surface::Asphalt, Surface::Ice);   // left grips, right is ice
    const double right = heading_change_braking_on_split(Surface::Ice, Surface::Asphalt);
    std::cout << "  heading change braking: even " << control << ", grip on the left " << left << ", grip on the right " << right << " degrees\n";
    require(std::fabs(control) < 0.2, "even ground: braking does not turn the car");
    require(left > 1.0, "grip on the left (+Y): the car yaws left");
    require(right < -1.0, "grip on the right: the car yaws right");
    require(std::fabs(left + right) < 0.25 * (std::fabs(left) + std::fabs(right)), "the two mirrored cases agree in magnitude");
}

void test_traction() {
    const auto speed_after = [](Surface s) {
        const Ground g = two_slabs(0, s, s);
        QueryScratch scratch;
        Vehicle v({}, {-100, 0, 1.0f}, 0.0f);
        drive(v, {1.0f, 0.0f, 0.0f}, 5.0, g, &g.surfaces, scratch);
        return static_cast<double>(v.state().forward_speed);
    };
    const double asphalt = speed_after(Surface::Asphalt), grass = speed_after(Surface::Grass), ice = speed_after(Surface::Ice);
    std::cout << "  speed after 5 s of full throttle: asphalt " << asphalt << ", grass " << grass << ", ice " << ice << " m/s\n";
    require(ice < 0.5 * asphalt, "on ice the same throttle gets under half the speed");
    require(grass < asphalt, "grass is slower than asphalt (rolling resistance)");
    require(ice > 0.0, "the car still moves on ice");
}

// Coasting (no throttle, no brake) from 15 m/s on one surface for 5 s: the speed lost to drag and rolling resistance.
double coast_loss_on(Surface surface) {
    const Ground g = two_slabs(0, Surface::Asphalt, surface);
    QueryScratch scratch;
    Vehicle v({}, {-100, 0, 1.0f}, 0.0f);
    VehicleInput in{1.0f, 0.0f, 0.0f};
    for (int i = 0; i < 60 * 12 && v.state().forward_speed < 15.0f; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    in = {0.0f, 0.0f, 0.0f};
    for (int i = 0; i < 60 * 20 && v.state().position.x < 0.0f; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    const double v0 = static_cast<double>(v.state().forward_speed);
    for (int i = 0; i < 60 * 5; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    return v0 - static_cast<double>(v.state().forward_speed);
}

void test_rolling_resistance() {
    const double asphalt = coast_loss_on(Surface::Asphalt), grass = coast_loss_on(Surface::Grass), sand = coast_loss_on(Surface::Sand);
    std::cout << "  speed lost coasting for 5 s: asphalt " << asphalt << ", grass " << grass << ", sand " << sand << " m/s\n";
    require(asphalt > 0.0, "drag and rolling resistance slow a coasting car");
    require(grass > 1.5 * asphalt && sand > grass, "rolling resistance grows from asphalt to grass to sand");
    require(sand > 3.0 * asphalt, "sand costs several times asphalt's coasting loss");
}

// A head-on crash (heading 0) or an angled one (the car drives at the wall at `heading` radians, so a front corner arrives first and the impact spins it) into a wall made of `surface` (the wall is box 1; the floor is box 0, asphalt).
struct Crash {
    double rebound;       // the most negative forward speed after the impact, m/s (a car bouncing back has a large negative one)
    double yaw_rate;      // the largest |yaw rate| after the impact, rad/s
    double approach;      // forward speed at the moment of impact
};
Crash crash_into(Surface surface, bool with_map, float heading = 0.0f) {
    std::vector<Aabb> boxes{{{-kHuge, -kHuge, -2}, {kHuge, kHuge, 0}}, {{30, -30, -2}, {31, 30, 6}}};
    StaticCollision world(boxes, {});
    SurfaceMap map(world, Surface::Asphalt);
    map.set({StaticPrimitive::Kind::Box, 1}, surface);
    QueryScratch scratch;
    Vehicle v({}, {0, 0, 1.0f}, heading);
    const SurfaceMap* m = with_map ? &map : nullptr;
    VehicleInput in{1.0f, 0.0f, 0.0f};
    for (int i = 0; i < 60 * 8 && v.state().forward_speed < 14.0f; ++i) v.step(in, 1.0f / 60.0f, world, scratch, m);
    in = {0.0f, 0.0f, 0.0f};
    Crash c{0.0, 0.0, static_cast<double>(v.state().forward_speed)};
    bool hit = false;
    for (int i = 0; i < 60 * 6; ++i) {
        v.step(in, 1.0f / 60.0f, world, scratch, m);
        if (!v.take_impacts().empty()) hit = true;
        if (hit) {
            c.rebound = std::min(c.rebound, static_cast<double>(v.state().forward_speed));
            c.yaw_rate = std::max(c.yaw_rate, static_cast<double>(std::fabs(v.state().angular_velocity.z)));
        }
    }
    require(hit, "the car hit the wall");
    return c;
}

void test_body_contacts_follow_the_surface() {
    // A head-on crash: a lively wall (rubber) throws the car back much harder than concrete does, and ice or snow no harder than the body itself would.
    const Crash none = crash_into(Surface::Concrete, false), concrete = crash_into(Surface::Concrete, true), rubber = crash_into(Surface::Rubber, true), snow = crash_into(Surface::Snow, true);
    std::cout << "  rebound speed after a head-on crash at " << none.approach << " m/s: no map " << none.rebound << ", concrete " << concrete.rebound << ", rubber " << rubber.rebound << ", snow " << snow.rebound << "\n";
    require(rubber.rebound < concrete.rebound - 1.5, "a rubber wall throws the car back harder than concrete");
    require(snow.rebound >= concrete.rebound - 0.3, "snow is no livelier than concrete");
    // An angled crash (a front corner first): friction at the wall spins the car; ice lets it slide along without much spin, rubber grips and spins it more.
    const Crash c_concrete = crash_into(Surface::Concrete, true, 0.35f), c_ice = crash_into(Surface::Ice, true, 0.35f), c_rubber = crash_into(Surface::Rubber, true, 0.35f);
    std::cout << "  peak yaw rate after an angled crash: ice " << c_ice.yaw_rate << ", concrete " << c_concrete.yaw_rate << ", rubber " << c_rubber.yaw_rate << " rad/s\n";
    require(c_ice.yaw_rate <= c_concrete.yaw_rate, "an icy wall spins the car no more than concrete");
    require(c_rubber.yaw_rate > c_concrete.yaw_rate * 1.08, "a grippy, lively wall spins the car clearly more than concrete");
}

} // namespace

int main() {
    try {
        test_table_and_map();
        test_no_map_is_the_old_behavior();
        test_stopping_distance_follows_grip();
        test_split_friction_yaws_toward_the_grippy_side();
        test_traction();
        test_rolling_resistance();
        test_body_contacts_follow_the_surface();
    } catch (const std::exception& error) {
        std::cerr << "vehicle surface test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "vehicle surface tests passed\n";
}
