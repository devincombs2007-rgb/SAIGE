// Persistent vehicle damage: a numeric state fed by the energy a collision takes out of the chassis and attributed by where the contact is, with physical consequences (a crumpled zone rebounds less, a hurt
// wheel grips less and toes out, a hurt suspension sags, a hurt engine delivers less), a versioned save format, and no change at all when it is off.
#include "saige/physics_material.hpp"
#include "saige/vehicle.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

struct World {
    StaticCollision world;
    SurfaceMap surfaces;
};
// Flat ground (a slab under z = 0) and a wall: x in [wall_x, wall_x + 1], y in [y0, y1], 5 m high, of `wall_surface`.
World with_wall(float wall_x, float y0, float y1, Surface wall_surface = Surface::Concrete) {
    std::vector<Aabb> boxes{{{-1000, -1000, -2}, {1000, 1000, 0}}, {{wall_x, y0, 0}, {wall_x + 1.0f, y1, 5}}};
    World w{StaticCollision(boxes, {}), {}};
    w.surfaces = SurfaceMap(w.world);
    w.surfaces.set({StaticPrimitive::Kind::Box, 0}, Surface::Asphalt);
    w.surfaces.set({StaticPrimitive::Kind::Box, 1}, wall_surface);
    return w;
}

VehicleConfig damaged_config(bool damage = true) {
    VehicleConfig c;
    c.powertrain.enabled = true;
    c.damage.enabled = damage;
    return c;
}

// Cruises at `target` toward +x (or -x in reverse) until the chassis front is `gap` from the wall, then lets go and returns what happened: the speed at the first impact event and the speed after it.
struct Hit {
    double approach = 0.0, rebound = 0.0, energy = 0.0;
    int events = 0;
};
Hit run_into_wall(Vehicle& v, const World& w, double target, bool reverse, double seconds_after = 1.5) {
    QueryScratch scratch;
    Hit hit;
    VehicleInput in;
    for (int i = 0; i < 60 * 40; ++i) {
        const double speed = reverse ? -static_cast<double>(v.state().forward_speed) : static_cast<double>(v.state().forward_speed);
        in.throttle = static_cast<float>(std::clamp((target - speed) * 0.6, -0.5, 1.0)) * (reverse ? -1.0f : 1.0f);
        v.step(in, 1.0f / 60.0f, w.world, scratch, &w.surfaces);
        for (const VehicleImpact& impact : v.take_impacts()) {
            ++hit.events;
            if (hit.events == 1) hit.approach = impact.speed;
            hit.energy += impact.energy;
        }
        if (hit.events > 0) break;
    }
    in = {};
    double peak_back = 0.0;
    for (int i = 0; i < static_cast<int>(seconds_after * 60.0); ++i) {
        v.step(in, 1.0f / 60.0f, w.world, scratch, &w.surfaces);
        for (const VehicleImpact& impact : v.take_impacts()) hit.energy += impact.energy;
        peak_back = std::max(peak_back, reverse ? static_cast<double>(v.state().forward_speed) : -static_cast<double>(v.state().forward_speed));
    }
    hit.rebound = peak_back;
    return hit;
}

bool same_state(const VehicleState& a, const VehicleState& b) {
    return std::memcmp(&a.position, &b.position, sizeof(Vec3)) == 0 && std::memcmp(&a.velocity, &b.velocity, sizeof(Vec3)) == 0 && std::memcmp(&a.orientation, &b.orientation, sizeof(VehicleQuat)) == 0 &&
           std::memcmp(&a.angular_velocity, &b.angular_velocity, sizeof(Vec3)) == 0;
}

bool rejects(const std::vector<std::uint8_t>& bytes) {
    try {
        (void)deserialize_damage(bytes);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void test_validation_and_format() {
    const auto bad = [](VehicleConfig x) {
        try {
            validate_vehicle_config(x);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    VehicleConfig c;
    c.damage.energy_capacity = 10.0f;
    require(bad(c), "a damage capacity of 10 J is rejected");
    c = {};
    c.damage.restitution_loss = 1.5f;
    require(bad(c), "a restitution loss above 1 is rejected");
    c = {};
    c.damage.alignment_pull = std::nanf("");
    require(bad(c), "a NaN alignment pull is rejected");
    require(!bad(damaged_config()), "the default damage configuration is valid");

    VehicleDamageState d;
    d.zone = {0.5f, 0.0f, 0.25f, 0.0f, 0.0f, 1.0f};
    d.wheel = {0.1f, 0.0f, 0.0f, 0.75f};
    d.suspension = {0.2f, 0.0f, 0.0f, 0.3f};
    d.engine = 0.4f;
    d.impacts = 3;
    d.absorbed_energy = 123456.789;
    const std::vector<std::uint8_t> bytes = serialize_damage(d);
    require(bytes.size() == 92, "a record is 92 bytes");
    require(deserialize_damage(bytes) == d, "a record round-trips exactly");
    require(serialize_damage(deserialize_damage(bytes)) == bytes, "and re-serializes to the same bytes");
    // A golden record of version 1 (written by this format's first release): a later build must still read it, or migrate it explicitly.
    const std::vector<std::uint8_t> golden = {0x53, 0x56, 0x44, 0x4d, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00};
    require(std::equal(golden.begin(), golden.end(), bytes.begin()), "the header is 'SVDM', version 1, six zones, four wheels");
    require(bytes[16] == 0x00 && bytes[17] == 0x00 && bytes[18] == 0x00 && bytes[19] == 0x3f, "the first zone, 0.5f, is little-endian IEEE-754 (3f000000)");
    // Every single-byte corruption is caught (the checksum, the header or the range check), and so is every truncation, an extension and a wrong version.
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        std::vector<std::uint8_t> broken = bytes;
        broken[i] ^= 0x40;
        require(rejects(broken), "a flipped bit in byte " + std::to_string(i) + " is rejected");
    }
    for (std::size_t n = 0; n < bytes.size(); ++n) require(rejects(std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + static_cast<long>(n))), "a truncated record is rejected");
    std::vector<std::uint8_t> longer = bytes;
    longer.push_back(0);
    require(rejects(longer), "a record with a trailing byte is rejected");
    std::vector<std::uint8_t> newer = bytes;
    newer[4] = 2;  // a version this build does not know; fix the checksum so only the version is wrong
    {
        std::uint32_t h = 2166136261u;
        for (std::size_t i = 0; i + 4 < newer.size(); ++i) h = (h ^ newer[i]) * 16777619u;
        for (int k = 0; k < 4; ++k) newer[newer.size() - 4 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>((h >> (8 * k)) & 0xff);
    }
    require(rejects(newer), "a newer version is refused with a valid checksum, not misread");
    // A record with a valid checksum but a value outside [0, 1] is refused (the range check, not only the checksum).
    {
        std::vector<std::uint8_t> out_of_range = bytes;
        const float two = 2.0f;
        std::memcpy(&out_of_range[16], &two, 4);  // the first zone (little-endian host assumed by the test; the format is little-endian by construction)
        std::uint32_t h = 2166136261u;
        for (std::size_t i = 0; i + 4 < out_of_range.size(); ++i) h = (h ^ out_of_range[i]) * 16777619u;
        for (int k = 0; k < 4; ++k) out_of_range[out_of_range.size() - 4 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>((h >> (8 * k)) & 0xff);
        require(rejects(out_of_range), "a value of 2.0 is refused even when the checksum is right");
    }
    VehicleDamageState nan_state = d;
    nan_state.engine = std::nanf("");
    bool threw = false;
    try {
        (void)serialize_damage(nan_state);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a NaN is not written");
    Vehicle v(damaged_config(), {0, 0, 0.9f}, 0.0f);
    threw = false;
    try {
        VehicleDamageState over = d;
        over.zone[0] = 1.5f;
        v.set_damage(over);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "set_damage refuses a value above 1");
    v.set_damage(d);
    require(v.damage() == d, "set_damage stores the state");
    v.repair();
    require(v.damage() == VehicleDamageState{}, "repair() restores a new vehicle");
}

void test_head_on() {
    double previous = 0.0;
    for (const double speed : {4.0, 8.0, 12.0}) {
        const World w = with_wall(0.0f, -50.0f, 50.0f);
        Vehicle v(damaged_config(), {-100, 0, 0.9f}, 0.0f);
        const Hit h = run_into_wall(v, w, speed, false);
        const VehicleDamageState& d = v.damage();
        const double expected = h.energy / static_cast<double>(v.config().damage.energy_capacity);
        std::printf("head-on %4.1f m/s: approach %.2f, rebound %.2f, collision loss %.0f J (%.0f%% of the kinetic energy); front %.3f (energy over capacity %.3f), engine %.2f, wheels %.2f %.2f, impacts %u\n", speed, h.approach, h.rebound, h.energy,
                    100.0 * h.energy / (0.5 * 1450.0 * speed * speed), d.zone[0], expected, d.engine, d.wheel[0], d.wheel[1], d.impacts);
        require(h.approach > speed - 0.3 && h.approach < speed + 0.1, "control: the car arrived at the speed asked");
        require(std::fabs(d.zone[0] - expected) < 0.04 * expected + 1e-4, "the front zone takes the energy the collision removed over the capacity");
        require(d.zone[0] > previous, "a harder hit damages the front more");
        previous = d.zone[0];
        require(d.zone[1] == 0.0f && d.zone[2] == 0.0f && d.zone[3] == 0.0f && d.zone[4] == 0.0f && d.zone[5] == 0.0f, "and nothing else");
        require(std::fabs(d.absorbed_energy - h.energy) < 1.0 && d.impacts >= 1, "the energy the model counted is the energy of the collision events");
        require(d.engine > 0.5 * d.zone[0] && d.engine < d.zone[0], "a front hit hurts the engine in proportion (0.8 of the share)");
    }
    require(previous > 0.5 && previous < 1.0, "control: at 12 m/s the front is badly damaged, not yet destroyed");
    // Saturation: harder than the capacity stays at 1.
    const World w = with_wall(0.0f, -50.0f, 50.0f);
    Vehicle v(damaged_config(), {-100, 0, 0.9f}, 0.0f);
    (void)run_into_wall(v, w, 22.0, false);
    require(v.damage().zone[0] == 1.0f && v.damage().engine == 1.0f, "a destroying hit saturates at 1");
    // A bump under the energy threshold does nothing; the same impact is still an event.
    VehicleConfig c = damaged_config();
    c.damage.min_event_energy = 20000.0f;
    Vehicle bump(c, {-100, 0, 0.9f}, 0.0f);
    const Hit h = run_into_wall(bump, w, 2.5, false);
    require(h.events >= 1 && h.energy < 20000.0, "control: a 2.5 m/s bump is reported as an impact event with less than the threshold");
    require(bump.damage() == VehicleDamageState{}, "and does no damage below the energy threshold");
}

void test_where() {
    // Reversing into a wall: the rear.
    {
        const World w = with_wall(-3.0f, -50.0f, 50.0f);  // behind a car at x = 30 ... the wall is at x in [-3, -2]
        Vehicle v(damaged_config(), {60, 0, 0.9f}, 0.0f);
        // The car faces +x and reverses toward -x; the wall's +x face is at x = -2.
        World behind = with_wall(-3.0f, -50.0f, 50.0f);
        const Hit h = run_into_wall(v, behind, 6.0, true);
        const VehicleDamageState& d = v.damage();
        std::printf("reversing into a wall at %.1f m/s: rear %.3f front %.3f left %.3f right %.3f\n", h.approach, d.zone[1], d.zone[0], d.zone[2], d.zone[3]);
        require(h.events >= 1 && d.zone[1] > 0.05, "the rear takes the hit when reversing into a wall");
        require(d.zone[0] == 0.0f && d.zone[2] == 0.0f && d.zone[3] == 0.0f, "and the other faces do not");
        require(d.engine == 0.0f, "the engine is only hurt by a front impact");
    }
    // The left front corner against the end of a wall (the wall covers y > 0.3, the car's left side).
    {
        const World w = with_wall(0.0f, 0.3f, 50.0f);
        Vehicle v(damaged_config(), {-100, 0.45f, 0.9f}, 0.0f);
        const Hit h = run_into_wall(v, w, 9.0, false);
        const VehicleDamageState& d = v.damage();
        std::printf("front-left corner at %.1f m/s: wheels FL %.3f FR %.3f RL %.3f RR %.3f, suspension FL %.3f FR %.3f, front %.3f\n", h.approach, d.wheel[0], d.wheel[1], d.wheel[2], d.wheel[3], d.suspension[0], d.suspension[1], d.zone[0]);
        require(h.events >= 1 && d.zone[0] > 0.1, "control: the corner hit damaged the front (a corner contact is on the front face)");
        require(d.wheel[0] > 3.0f * d.wheel[1] + 0.005f && d.suspension[0] > 3.0f * d.suspension[1] + 0.005f, "the wheel and suspension on the side that hit take far more than the far one");
        require(d.wheel[2] == 0.0f && d.wheel[3] == 0.0f && d.suspension[2] == 0.0f && d.suspension[3] == 0.0f, "and the rear wheels none");
    }
    // A hard landing on the underbody: dropped onto the ground from 4 m with the suspension unable to take it.
    {
        const World w = with_wall(900.0f, -1.0f, 1.0f);
        Vehicle v(damaged_config(), {0, 0, 9.0f}, 0.0f);
        QueryScratch scratch;
        VehicleInput in;
        for (int i = 0; i < 60 * 3; ++i) {
            v.step(in, 1.0f / 60.0f, w.world, scratch, &w.surfaces);
            (void)v.take_impacts();
        }
        const VehicleDamageState& d = v.damage();
        std::printf("dropped from 8 m: underbody %.3f front %.3f rear %.3f roof %.3f impacts %u\n", d.zone[5], d.zone[0], d.zone[1], d.zone[4], d.impacts);
        require(d.zone[5] > 0.1 && d.zone[0] == 0.0f && d.zone[1] == 0.0f && d.zone[4] == 0.0f, "a hard landing damages the underbody and nothing else");
    }
    // A spin-out into a wall: the car turns in hard at 22 m/s with the slip-angle tires and is not caught, slides sideways to the guard rails 9 m either side and hits one with its side.
    {
        std::vector<Aabb> boxes{{{-1000, -1000, -2}, {1000, 1000, 0}}, {{-300, 9.0f, 0}, {300, 10.0f, 5}}, {{-300, -10.0f, 0}, {300, -9.0f, 5}}};
        World w{StaticCollision(boxes, {}), {}};
        w.surfaces = SurfaceMap(w.world);
        VehicleConfig c = damaged_config();
        c.slip_angle_lateral = true;
        c.differential = VehicleDifferential::LimitedSlip;
        Vehicle v(c, {-240, 0, 0.9f}, 0.0f);
        QueryScratch scratch;
        VehicleInput in;
        double side_speed = 0.0;
        for (int i = 0; i < 60 * 12; ++i) {
            const double t = i / 60.0;
            in.throttle = static_cast<float>(std::clamp((22.0 - v.state().forward_speed) * 0.6, -0.5, 1.0));
            if (t > 4.0) in.throttle = 1.0f;
            in.steer = (t > 4.0 && t < 6.0) ? 0.7f : 0.0f;
            v.step(in, 1.0f / 60.0f, w.world, scratch, &w.surfaces);
            for (const VehicleImpact& im : v.take_impacts()) side_speed = std::max(side_speed, static_cast<double>(im.speed));
        }
        const VehicleDamageState& d = v.damage();
        std::printf("spin-out into the rails: worst impact %.2f m/s; zones front %.3f rear %.3f left %.3f right %.3f roof %.3f under %.3f; impacts %u\n", side_speed, d.zone[0], d.zone[1], d.zone[2], d.zone[3], d.zone[4], d.zone[5], d.impacts);
        require(side_speed > 5.0 && d.impacts >= 2, "control: the spin-out hit the rails hard, more than once");
        require(d.zone[2] > 0.3 && d.zone[2] > 3.0f * d.zone[3], "the side that met the rail takes the damage: left far above right");
    }
}

void test_consequences() {
    const World w = with_wall(0.0f, -50.0f, 50.0f);
    // A crumpled front gives back less: the same 8 m/s hit rebounds a fraction as fast.
    {
        Vehicle fresh(damaged_config(), {-100, 0, 0.9f}, 0.0f), crumpled(damaged_config(), {-100, 0, 0.9f}, 0.0f);
        VehicleDamageState d;
        d.zone[0] = 1.0f;
        crumpled.set_damage(d);
        const Hit a = run_into_wall(fresh, w, 8.0, false), b = run_into_wall(crumpled, w, 8.0, false);
        std::printf("rebound at 8 m/s: new front %.2f m/s, destroyed front %.2f m/s\n", a.rebound, b.rebound);
        require(a.rebound > 0.9, "control: a new car rebounds");
        require(b.rebound < 0.45 * a.rebound, "a car with a destroyed front rebounds at under 0.45 of that (its restitution fell by 0.7)");
        require(b.energy > a.energy, "and loses more of its energy in the collision");
    }
    const World flat = with_wall(900.0f, -1.0f, 1.0f);
    QueryScratch scratch;
    // A bent front-left wheel toes out and pulls the car: a straight run at 25 m/s with the wheel at 1.0 veers, the new car does not.
    {
        const auto veer = [&](float wheel_damage) {
            Vehicle v(damaged_config(), {-240, 0, 0.9f}, 0.0f);
            VehicleDamageState d;
            d.wheel[0] = wheel_damage;
            v.set_damage(d);
            VehicleInput in;
            for (int i = 0; i < 60 * 40 && v.state().forward_speed < 25.0f; ++i) {
                in.throttle = 1.0f;
                v.step(in, 1.0f / 60.0f, flat.world, scratch, &flat.surfaces);
            }
            in.throttle = 0.2f;
            const double y0 = v.state().position.y, yaw0 = v.state().yaw;
            for (int i = 0; i < 60 * 3; ++i) v.step(in, 1.0f / 60.0f, flat.world, scratch, &flat.surfaces);
            return std::pair<double, double>{v.state().position.y - y0, (v.state().yaw - yaw0) * 57.29578};
        };
        const auto fresh = veer(0.0f), bent = veer(1.0f), half = veer(0.5f);
        std::printf("3 s straight at 25 m/s with the front-left wheel at 0 / 0.5 / 1: sideways %.2f / %.2f / %.2f m, heading %.2f / %.2f / %.2f deg\n", fresh.first, half.first, bent.first, fresh.second, half.second, bent.second);
        require(std::fabs(fresh.first) < 0.1 && std::fabs(fresh.second) < 0.1, "control: the new car runs straight");
        require(bent.first > 0.5 && bent.second > 1.0, "a destroyed wheel pulls the car to the left (the side of the bent wheel)");
        {
            Vehicle v(damaged_config(), {-240, 0, 0.9f}, 0.0f);
            VehicleDamageState d;
            d.wheel[1] = 1.0f;  // the front-right wheel: it pulls the other way
            v.set_damage(d);
            VehicleInput in;
            for (int i = 0; i < 60 * 40 && v.state().forward_speed < 25.0f; ++i) {
                in.throttle = 1.0f;
                v.step(in, 1.0f / 60.0f, flat.world, scratch, &flat.surfaces);
            }
            in.throttle = 0.2f;
            const double y0 = v.state().position.y;
            for (int i = 0; i < 60 * 3; ++i) v.step(in, 1.0f / 60.0f, flat.world, scratch, &flat.surfaces);
            std::printf("front-right wheel destroyed: sideways %.2f m\n", v.state().position.y - y0);
            require(v.state().position.y - y0 < -0.5, "a destroyed front-right wheel pulls the car to the right: the toe is outward on each side");
        }
        require(half.first > 0.5 && half.second > 1.0, "and half the damage pulls the same way (not in proportion: the tire force saturates, so the model claims no linear law)");
    }
    // A hurt wheel grips less: braking from 20 m/s takes much further with every wheel at 1.0.
    {
        const auto stop = [&](float wheel_damage) {
            VehicleConfig c = damaged_config();
            Vehicle v(c, {-240, 0, 0.9f}, 0.0f);
            VehicleDamageState d;
            d.wheel = {wheel_damage, wheel_damage, wheel_damage, wheel_damage};
            v.set_damage(d);
            VehicleInput in;
            for (int i = 0; i < 60 * 40 && v.state().forward_speed < 20.0f; ++i) {
                in.throttle = 1.0f;
                v.step(in, 1.0f / 60.0f, flat.world, scratch, &flat.surfaces);
            }
            const double x0 = v.state().position.x;
            in = {};
            in.brake = 1.0f;
            for (int i = 0; i < 60 * 20 && v.state().forward_speed > 0.1f; ++i) v.step(in, 1.0f / 60.0f, flat.world, scratch, &flat.surfaces);
            return v.state().position.x - x0;
        };
        const double good = stop(0.0f), hurt = stop(1.0f);
        std::printf("braking from 20 m/s: %.1f m new, %.1f m with every wheel destroyed\n", good, hurt);
        require(hurt > 1.5 * good, "destroyed wheels take at least half as far again to stop");
    }
    // A hurt suspension is softer: it sags at that corner.
    {
        const auto settled = [&](float suspension_damage) {
            Vehicle v(damaged_config(), {0, 0, 0.9f}, 0.0f);
            VehicleDamageState d;
            d.suspension[0] = suspension_damage;
            v.set_damage(d);
            VehicleInput in;
            for (int i = 0; i < 60 * 4; ++i) v.step(in, 1.0f / 60.0f, flat.world, scratch, &flat.surfaces);
            return v.state().wheels[0].compression;
        };
        const double good = settled(0.0f), hurt = settled(1.0f);
        std::printf("static compression at the front-left wheel: %.3f m new, %.3f m with the suspension destroyed\n", good, hurt);
        require(hurt > 1.5 * good, "a destroyed suspension sags at its corner: far more compression under the same weight");
    }
    // A hurt engine delivers less: the time to 20 m/s.
    {
        const auto time_to = [&](float engine_damage) {
            Vehicle v(damaged_config(), {-240, 0, 0.9f}, 0.0f);
            VehicleDamageState d;
            d.engine = engine_damage;
            v.set_damage(d);
            VehicleInput in;
            in.throttle = 1.0f;
            int i = 0;
            for (; i < 60 * 40 && v.state().forward_speed < 20.0f; ++i) v.step(in, 1.0f / 60.0f, flat.world, scratch, &flat.surfaces);
            return i / 60.0;
        };
        const double good = time_to(0.0f), half = time_to(0.5f), dead = time_to(1.0f);
        std::printf("time to 20 m/s with the engine at 0 / 0.5 / 1: %.2f / %.2f / %.2f s\n", good, half, dead);
        require(half > 1.2 * good && dead > 1.5 * good && dead > half, "a hurt engine accelerates slower, more so when more hurt");
        Vehicle v(damaged_config(), {-240, 0, 0.9f}, 0.0f);
        VehicleDamageState d;
        d.engine = 1.0f;
        v.set_damage(d);
        v.repair();
        require(std::fabs(time_to(0.0f) - good) < 1e-9, "control: repairing restores the new car's acceleration");
    }
}

void test_materials() {
    // The same collision against a rubber barrier and a concrete wall at three speeds: rubber gives back more and so takes less out of the car.
    for (const double speed : {5.0, 9.0, 13.0}) {
        const World concrete = with_wall(0.0f, -50.0f, 50.0f, Surface::Concrete), rubber = with_wall(0.0f, -50.0f, 50.0f, Surface::Rubber);
        Vehicle a(damaged_config(), {-100, 0, 0.9f}, 0.0f), b(damaged_config(), {-100, 0, 0.9f}, 0.0f);
        const Hit hc = run_into_wall(a, concrete, speed, false), hr = run_into_wall(b, rubber, speed, false);
        std::printf("%.0f m/s: concrete rebound %.2f m/s, loss %.0f J, front %.3f | rubber rebound %.2f m/s, loss %.0f J, front %.3f\n", speed, hc.rebound, hc.energy, a.damage().zone[0], hr.rebound, hr.energy, b.damage().zone[0]);
        require(hr.rebound > 2.0 * hc.rebound, "rubber sends the car back at more than twice the speed of concrete");
        require(hr.energy < hc.energy && b.damage().zone[0] < a.damage().zone[0], "and takes less out of it, so the damage is less");
        require(std::fabs(hc.rebound / hc.approach - 0.2) < 0.08, "concrete returns about its restitution of the approach speed");
    }
}

void test_off_and_neutral() {
    const World w = with_wall(0.0f, -50.0f, 50.0f);
    // Disabled: a collision leaves the damage state empty and the vehicle exactly as one with no damage configuration at all.
    {
        VehicleConfig plain;
        plain.powertrain.enabled = true;
        Vehicle a(plain, {-100, 0, 0.9f}, 0.0f), b(damaged_config(false), {-100, 0, 0.9f}, 0.0f);
        (void)run_into_wall(a, w, 10.0, false);
        (void)run_into_wall(b, w, 10.0, false);
        require(b.damage() == VehicleDamageState{}, "with damage disabled a collision records nothing");
        require(same_state(a.state(), b.state()), "and the run is bit-identical to a configuration that never mentioned damage");
    }
    // Enabled but undamaged and not colliding: the arithmetic is the same (every factor is exactly 1).
    {
        const World flat = with_wall(900.0f, -1.0f, 1.0f);
        QueryScratch scratch;
        Vehicle a(damaged_config(false), {-240, 0, 0.9f}, 0.0f), b(damaged_config(true), {-240, 0, 0.9f}, 0.0f);
        VehicleInput in;
        for (int i = 0; i < 900; ++i) {
            in.throttle = i < 600 ? 1.0f : 0.0f;
            in.steer = std::sin(static_cast<float>(i) * 0.04f);
            in.brake = i > 700 ? 0.4f : 0.0f;
            a.step(in, 1.0f / 60.0f, flat.world, scratch, &flat.surfaces);
            b.step(in, 1.0f / 60.0f, flat.world, scratch, &flat.surfaces);
        }
        require(same_state(a.state(), b.state()) && a.state().forward_speed > 2.0f, "enabled with no damage, a drive is bit-identical to one with damage off");
    }
}

void test_sustained_driving() {
    // Two minutes of varied driving with every option on: no NaN, the car stays on the ground at plausible speeds, and a second run is bit-identical. The scripted driver follows a figure of eight at a
    // speed that keeps changing, with brake and handbrake use and a crash into a wall at the end.
    const auto run = [](VehicleState& out, VehicleDamageState& damage, double& worst_speed) {
        std::vector<Aabb> boxes{{{-1000, -1000, -2}, {1000, 1000, 0}}, {{150, -60, 0}, {151, 60, 5}}};
        World w{StaticCollision(boxes, {}), {}};
        w.surfaces = SurfaceMap(w.world);
        w.surfaces.set({StaticPrimitive::Kind::Box, 0}, Surface::Asphalt);
        VehicleConfig c = damaged_config();
        c.differential = VehicleDifferential::LimitedSlip;
        c.slip_angle_lateral = true;
        c.powertrain.engine_brake = 90.0f;
        c.tire_load_sensitivity = 0.2f;
        Vehicle v(c, {0, 0, 0.9f}, 0.0f);
        QueryScratch scratch;
        VehicleInput in;
        worst_speed = 0.0;
        for (int i = 0; i < 60 * 120; ++i) {
            const double t = i / 60.0;
            const double target = 8.0 + 6.0 * std::sin(t * 0.21);
            in.throttle = static_cast<float>(std::clamp((target - v.state().forward_speed) * 0.5, -1.0, 1.0));
            in.brake = in.throttle < -0.2f ? 0.5f : 0.0f;
            if (in.throttle < 0.0f) in.throttle = 0.0f;
            in.steer = static_cast<float>(0.4 * std::sin(t * 0.5));
            {
                // Stay inside the supported +-256 m: past 90 m from the origin steer back toward it.
                const double px = v.state().position.x, py = v.state().position.y;
                if (std::hypot(px, py) > 90.0) {
                    double err = std::atan2(-py, -px) - static_cast<double>(v.state().yaw);
                    while (err > 3.14159265) err -= 6.2831853;
                    while (err < -3.14159265) err += 6.2831853;
                    in.steer = static_cast<float>(std::clamp(err * 1.2, -1.0, 1.0));
                }
            }
            in.handbrake = (std::fmod(t, 23.0) > 21.8) ? 0.6f : 0.0f;
            if (t > 105.0) {  // aim at the wall (x = 150) from wherever it is
                double err = std::atan2(-static_cast<double>(v.state().position.y), 150.0 - static_cast<double>(v.state().position.x)) - static_cast<double>(v.state().yaw);
                while (err > 3.14159265) err -= 6.2831853;
                while (err < -3.14159265) err += 6.2831853;
                in.steer = static_cast<float>(std::clamp(err * 1.5, -1.0, 1.0));
                in.throttle = 1.0f;
                in.brake = 0.0f;
                in.handbrake = 0.0f;
            }
            v.step(in, 1.0f / 60.0f, w.world, scratch, &w.surfaces);
            (void)v.take_impacts();
            const VehicleState& s = v.state();
            require(std::isfinite(s.position.x) && std::isfinite(s.position.y) && std::isfinite(s.position.z) && std::isfinite(s.forward_speed), "the state stays finite");
            require(s.position.z > -0.5f && s.position.z < 3.0f, "and the car stays on the ground");
            worst_speed = std::max(worst_speed, static_cast<double>(std::fabs(s.forward_speed)));
        }
        out = v.state();
        damage = v.damage();
    };
    VehicleState s1, s2;
    VehicleDamageState d1, d2;
    double w1 = 0.0, w2 = 0.0;
    run(s1, d1, w1);
    run(s2, d2, w2);
    std::printf("two minutes of driving: top speed %.1f m/s, damage at the end: front %.3f (rear %.3f left %.3f right %.3f roof %.3f under %.3f) impacts %u, end position %.1f %.1f speed %.1f\n", w1, d1.zone[0], d1.zone[1], d1.zone[2], d1.zone[3], d1.zone[4], d1.zone[5], d1.impacts, static_cast<double>(s1.position.x), static_cast<double>(s1.position.y), static_cast<double>(s1.forward_speed));
    require(same_state(s1, s2) && d1 == d2, "a second run is bit-identical, state and damage");
    require(d1.impacts >= 1 && d1.zone[0] + d1.zone[1] + d1.zone[2] + d1.zone[3] > 0.2, "control: the run ended in a collision, so the damage path ran inside the long drive");
    require(w1 > 10.0 && w1 < 60.0, "control: the driver actually drove (top speed between 10 and 60 m/s)");
}

} // namespace

int main(int argc, char** argv) {
    struct Named {
        const char* name;
        void (*fn)();
    };
    const Named tests[] = {{"format", test_validation_and_format}, {"head_on", test_head_on}, {"where", test_where}, {"consequences", test_consequences},
                           {"materials", test_materials},         {"off", test_off_and_neutral}, {"sustained", test_sustained_driving}};
    int failures = 0;
    for (const Named& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;  // a single test by name (the mutation checks run them one at a time)
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cerr << "FAIL (" << t.name << "): " << e.what() << "\n";
            ++failures;
        }
    }
    if (failures == 0) std::cout << "vehicle damage tests passed\n";
    return failures == 0 ? 0 : 1;
}
