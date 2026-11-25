// Vehicle drivetrain and tire options: the differential (an open one starves on the weak wheel, a locked or limited-slip one does not), engine braking when the throttle is released, and the slip-angle lateral
// tire (a tail that can stay out in a controlled drift). All are opt-in; a configuration with them neutral is bit-identical to one without them.
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

constexpr float kHuge = 5000.0f;

struct Ground {
    StaticCollision world;
    SurfaceMap surfaces;
};
// Flat ground; with `split`, the left half (y > 0) is `left` and the right half `right`.
Ground ground(Surface left, Surface right) {
    std::vector<Aabb> boxes{{{-kHuge, 0, -2}, {kHuge, kHuge, 0}}, {{-kHuge, -kHuge, -2}, {kHuge, 0, 0}}};
    Ground g{StaticCollision(boxes, {}), {}};
    g.surfaces = SurfaceMap(g.world);
    g.surfaces.set({StaticPrimitive::Kind::Box, 0}, left);
    g.surfaces.set({StaticPrimitive::Kind::Box, 1}, right);
    return g;
}

void drive(Vehicle& v, const VehicleInput& in, double seconds, const Ground& g, QueryScratch& scratch) {
    const int ticks = static_cast<int>(std::lround(seconds * 60.0));
    for (int i = 0; i < ticks; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
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
    const auto bad = [](VehicleConfig x) {
        try {
            validate_vehicle_config(x);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    VehicleConfig c;
    c.lsd_bias = 0.5f;
    require(bad(c), "an LSD bias below 1 is rejected");
    c = {};
    c.lsd_preload = -1.0f;
    require(bad(c), "a negative preload is rejected");
    c = {};
    c.lateral_slide_ratio = 0.1f;
    require(bad(c), "a slide ratio of 0.1 is rejected");
    c = {};
    c.lateral_stiffness = std::nanf("");
    require(bad(c), "a NaN lateral stiffness is rejected");
    c = {};
    c.differential = static_cast<VehicleDifferential>(9);
    require(bad(c), "an unknown differential is rejected");
    c = {};
    c.powertrain.enabled = true;
    c.powertrain.engine_brake = -3.0f;
    require(bad(c), "a negative engine braking torque is rejected");
    c.powertrain.engine_brake = 90.0f;
    c.differential = VehicleDifferential::LimitedSlip;
    c.slip_angle_lateral = true;
    require(!bad(c), "a valid drivetrain configuration is accepted");
}

// Neutral settings change nothing: the new options at their defaults (and the unused parameters set to other values) reproduce the original arithmetic bit for bit.
void test_neutral_is_bit_identical() {
    const Ground g = ground(Surface::Asphalt, Surface::Asphalt);
    QueryScratch scratch;
    VehicleConfig a;
    a.powertrain.enabled = true;
    VehicleConfig b = a;
    b.lsd_bias = 7.0f;
    b.lsd_preload = 1234.0f;
    b.lateral_stiffness = 33.0f;
    b.lateral_slide_ratio = 0.5f;
    Vehicle va(a, {-240, 0, 0.9f}, 0.0f), vb(b, {-240, 0, 0.9f}, 0.0f);
    VehicleInput in;
    for (int i = 0; i < 600; ++i) {
        in.throttle = i < 400 ? 1.0f : 0.0f;
        in.steer = std::sin(static_cast<float>(i) * 0.05f);
        in.brake = i > 500 ? 0.5f : 0.0f;
        va.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
        vb.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    }
    require(same_state(va.state(), vb.state()), "unused drivetrain parameters leave the simulation bit-identical");
    require(va.state().forward_speed > 5.0f, "control: the car moved");
}

struct Run {
    double speed, distance, yaw_deg, drive_left, drive_right;
};
Run launch(VehicleDifferential diff, Surface left, Surface right) {
    const Ground g = ground(left, right);
    QueryScratch scratch;
    VehicleConfig cfg;
    cfg.differential = diff;
    cfg.powertrain.enabled = true;
    Vehicle v(cfg, {-100, 0, 0.9f}, 0.0f);
    VehicleInput in;
    in.throttle = 1.0f;
    drive(v, in, 1.5, g, scratch);
    // The rear wheels are 2 (left) and 3 (right).
    const double dl = v.state().wheels[2].drive_force, dr = v.state().wheels[3].drive_force;
    drive(v, in, 1.5, g, scratch);
    return {v.state().forward_speed, v.state().position.x + 100.0, std::fabs(v.state().yaw) * 57.29578, dl, dr};
}

void test_asymmetric_traction() {
    const Run sym_legacy = launch(VehicleDifferential::Legacy, Surface::Asphalt, Surface::Asphalt);
    const Run sym_open = launch(VehicleDifferential::Open, Surface::Asphalt, Surface::Asphalt);
    const Run legacy = launch(VehicleDifferential::Legacy, Surface::Ice, Surface::Asphalt);
    const Run open = launch(VehicleDifferential::Open, Surface::Ice, Surface::Asphalt);
    const Run locked = launch(VehicleDifferential::Locked, Surface::Ice, Surface::Asphalt);
    const Run lsd = launch(VehicleDifferential::LimitedSlip, Surface::Ice, Surface::Asphalt);
    std::printf("split traction (ice left / asphalt right), 3 s of full throttle: symmetric legacy %.1f m/s, symmetric open %.1f\n", sym_legacy.speed, sym_open.speed);
    std::printf("  legacy %.2f m/s %.1f m   open %.2f m/s %.1f m   locked %.2f m/s %.1f m   lsd %.2f m/s %.1f m\n", legacy.speed, legacy.distance, open.speed, open.distance, locked.speed, locked.distance, lsd.speed, lsd.distance);
    std::printf("  rear drive force (L ice / R asphalt): legacy %.0f / %.0f  open %.0f / %.0f  locked %.0f / %.0f  lsd %.0f / %.0f N; heading change %.1f %.1f %.1f %.1f deg\n", legacy.drive_left, legacy.drive_right,
                open.drive_left, open.drive_right, locked.drive_left, locked.drive_right, lsd.drive_left, lsd.drive_right, legacy.yaw_deg, open.yaw_deg, locked.yaw_deg, lsd.yaw_deg);
    require(std::fabs(sym_open.speed - sym_legacy.speed) < 0.03 * sym_legacy.speed, "on even grip an open differential accelerates like the original model");
    require(sym_open.speed > 14.0, "control: the car accelerates on even grip");
    // Open: the same force at both wheels, which is all the icy wheel can carry.
    require(std::fabs(open.drive_left - open.drive_right) < 0.05 * std::max(open.drive_left, open.drive_right), "an open differential gives both wheels the same force");
    require(open.drive_right < 450.0, "and that force is what the icy wheel's grip allows (about 0.1 mu of the load), not what the asphalt could carry");
    require(open.speed < 3.0, "so an open differential barely moves the car");
    // The original model clips each wheel on its own, as an idealised locked axle does.
    require(legacy.drive_right > 4000.0 && legacy.speed > 8.0, "control: with per-wheel clipping the asphalt wheel pulls the car");
    require(locked.speed > 6.0 && locked.drive_right > 4000.0, "a locked differential sends the force to the wheel that can use it");
    require(locked.drive_right > 15.0 * locked.drive_left && locked.drive_left < 0.6 * legacy.drive_left, "in proportion to each wheel's grip: the icy wheel gets less than the per-wheel clipping of the original model gave it");
    require(lsd.speed > 1.8 * open.speed && lsd.speed < 0.7 * locked.speed, "a limited-slip differential is in between");
    require(lsd.drive_right > 2.0 * lsd.drive_left && lsd.drive_right < 2.5 * (lsd.drive_left + 40.0) + 300.0 + 100.0, "its strong wheel takes at most the bias times the weak one plus the preload");
    require(open.speed < lsd.speed && lsd.speed < locked.speed, "open < limited slip < locked");
    for (const Run* r : {&open, &locked, &lsd, &legacy}) require(r->yaw_deg < 12.0, "the car holds its heading while the force is shared unevenly");
}

struct Coast {
    double decel;  // m/s^2 over the window
    double rpm_first;
    double rear_force;  // N, the two driven wheels' longitudinal force on the first tick of the window
    int gear;
    double rpm_mean;
    double start_speed;
};
// A car accelerated to `speed` on the flat, then the throttle released: the mean deceleration over the next `window` seconds.
Coast release(float engine_brake, double speed, double window, double start_speed_after = 0.0) {
    (void)start_speed_after;
    const Ground g = ground(Surface::Asphalt, Surface::Asphalt);
    QueryScratch scratch;
    VehicleConfig cfg;
    cfg.powertrain.enabled = true;
    cfg.powertrain.engine_brake = engine_brake;
    Vehicle v(cfg, {-240, 0, 0.9f}, 0.0f);
    VehicleInput gas;
    gas.throttle = 1.0f;
    for (int i = 0; i < 60 * 60 && v.state().forward_speed < speed; ++i) v.step(gas, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    const VehicleInput off;
    drive(v, off, 0.3, g, scratch);  // the shift (if any) finishes and the engine settles
    v.step(off, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    const double rear_force = static_cast<double>(v.state().wheels[2].drive_force) + static_cast<double>(v.state().wheels[3].drive_force);
    const double rpm_first = v.state().engine_rpm;
    const double v0 = v.state().forward_speed;
    const int gear = v.state().gear;
    double rpm_sum = 0.0;
    const int ticks = static_cast<int>(std::lround(window * 60.0));
    for (int i = 0; i < ticks; ++i) {
        v.step(off, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
        rpm_sum += v.state().engine_rpm;
    }
    return {(v0 - v.state().forward_speed) / window, rpm_first, rear_force, gear, rpm_sum / ticks, v0};
}

// The deceleration the engine's drag torque adds, from the configuration: torque at the redline scaled by rpm, through the gear and final drive, over the wheel radius and the mass.
double predicted_engine_decel(float engine_brake, int gear, double rpm) {
    const VehicleConfig cfg;
    const VehiclePowertrain pt;
    const double ratio = static_cast<double>(pt.gear_ratios[static_cast<std::size_t>(gear - 1)]) * static_cast<double>(pt.final_drive);
    return engine_brake * rpm / static_cast<double>(pt.redline_rpm) * ratio * static_cast<double>(pt.drivetrain_efficiency) / static_cast<double>(cfg.wheel_radius) / static_cast<double>(cfg.mass);
}

void test_engine_braking() {
    for (const double speed : {28.0, 12.0}) {
        const Coast none = release(0.0f, speed, 0.5), eb = release(120.0f, speed, 0.5);
        const double extra = eb.decel - none.decel;
        const double expected = predicted_engine_decel(120.0f, eb.gear, eb.rpm_mean);
        std::printf("engine braking at ~%.0f m/s (gear %d, %.0f rpm): coasting %.3f m/s^2, with 120 N m %.3f; extra %.3f, predicted %.3f\n", speed, eb.gear, eb.rpm_mean, none.decel, eb.decel, extra, expected);
        // The force itself, on one tick: the drag torque through the gear, final drive and efficiency over the wheel radius (the gear and engine speed are the published ones).
        const double force_extra = -(eb.rear_force - none.rear_force);
        const double force_expected = predicted_engine_decel(120.0f, eb.gear, eb.rpm_first) * static_cast<double>(VehicleConfig{}.mass);
        require(std::fabs(force_extra - force_expected) < 0.03 * force_expected, "the force on the driven wheels is the drag torque times the ratio and efficiency over the radius, within 3%");
        require(none.decel < 0.6, "control: a car coasting with the throttle released loses little speed (drag and rolling)");
        require(extra > 0.5, "releasing the throttle slows a car with engine braking much more");
        require(std::fabs(extra - expected) < 0.08 * expected, "and by the deceleration the drag torque gives through the gear (the aerodynamic drag it takes away as the car slows is the few percent difference)");
    }
    // In a low gear the same drag torque brakes harder.
    const Coast high = release(120.0f, 28.0, 0.5), low = release(120.0f, 12.0, 0.5);
    const double high_extra = high.decel - release(0.0f, 28.0, 0.5).decel, low_extra = low.decel - release(0.0f, 12.0, 0.5).decel;
    require(low.gear < high.gear && low_extra > 2.0 * high_extra, "a lower gear brakes harder (the ratio of the gears' ratios, about 2.4)");
    // With the throttle held the engine does not brake: the run is bit-identical to one without engine braking.
    {
        const Ground g = ground(Surface::Asphalt, Surface::Asphalt);
        QueryScratch scratch;
        VehicleConfig a, b;
        a.powertrain.enabled = b.powertrain.enabled = true;
        b.powertrain.engine_brake = 200.0f;
        Vehicle va(a, {-240, 0, 0.9f}, 0.0f), vb(b, {-240, 0, 0.9f}, 0.0f);
        VehicleInput in;
        in.throttle = 0.4f;
        for (int i = 0; i < 600; ++i) {
            va.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
            vb.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
        }
        require(same_state(va.state(), vb.state()), "with the throttle held engine braking changes nothing");
    }
    // Below a walking pace the clutch is out: a car at rest does not creep backward or forward on the flat, and one rolling at 0.4 m/s gets no extra drag from it.
    {
        const Ground g = ground(Surface::Asphalt, Surface::Asphalt);
        QueryScratch scratch;
        VehicleConfig b;
        b.powertrain.enabled = true;
        b.powertrain.engine_brake = 400.0f;
        Vehicle v(b, {-240, 0, 0.9f}, 0.0f);
        drive(v, VehicleInput{}, 3.0, g, scratch);
        require(std::fabs(v.state().forward_speed) < 0.01f, "a car at rest with engine braking stays at rest");
        // Rolling out below a walking pace (0.5 m/s) the clutch is out: no engine braking force is left on the wheels (only rolling resistance, 35 N a wheel).
        Vehicle w(b, {-240, 0, 0.9f}, 0.0f);
        VehicleInput gas;
        gas.throttle = 1.0f;
        for (int i = 0; i < 60 * 30 && w.state().forward_speed < 6.0f; ++i) w.step(gas, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
        const VehicleInput off;
        for (int i = 0; i < 60 * 30 && w.state().forward_speed > 0.4f; ++i) w.step(off, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
        const double rear = static_cast<double>(w.state().wheels[2].drive_force) + static_cast<double>(w.state().wheels[3].drive_force);
        require(w.state().forward_speed > 0.0f && w.state().forward_speed <= 0.4f, "control: the car rolled out to 0.4 m/s");
        require(std::fabs(rear) < 120.0, "below a walking pace there is no engine braking force");
    }
}

void test_tire_curve() {
    const double pi = 3.14159265358979323846;
    for (const double ratio : {1.0, 0.85, 0.6, 0.3}) {
        for (const double b : {4.0, 12.0, 40.0}) {
            const double c = 2.0 / pi * (pi - std::asin(ratio));
            require(tire_lateral_ratio(0.0, b, ratio) == 0.0, "no slip, no lateral force");
            const double small = 1e-4;
            require(std::fabs(tire_lateral_ratio(small, b, ratio) / small - c * b) < 0.002 * c * b, "the initial slope is C times the stiffness");
            require(tire_lateral_ratio(0.2, b, ratio) == tire_lateral_ratio(-0.2, b, ratio), "even in the slip angle");
            if (ratio == 1.0) {
                require(tire_lateral_ratio(0.1, b, ratio) < tire_lateral_ratio(0.5, b, ratio) && tire_lateral_ratio(1.5, b, ratio) <= 1.0, "a slide ratio of 1 never falls: it rises to the limit and stays");
                continue;
            }
            const double peak_angle = std::tan(pi / (2.0 * c)) / b;
            require(std::fabs(tire_lateral_ratio(peak_angle, b, ratio) - 1.0) < 1e-9, "the peak is 1 at the angle that follows from the stiffness and shape");
            require(tire_lateral_ratio(peak_angle * 0.5, b, ratio) < tire_lateral_ratio(peak_angle * 0.9, b, ratio) && tire_lateral_ratio(peak_angle * 0.9, b, ratio) < 1.0, "it rises up to the peak");
            require(std::fabs(tire_lateral_ratio(1e9, b, ratio) - ratio) < 1e-6, "and falls to the slide ratio as the angle grows");
            require(tire_lateral_ratio(peak_angle * 3.0, b, ratio) < 1.0 - 0.005 && tire_lateral_ratio(1.5, b, ratio) < tire_lateral_ratio(peak_angle * 1.2, b, ratio), "past the peak the force falls");
        }
    }
    std::printf("tire curve (B 12, slide ratio 0.85): peak at %.1f deg, %.3f at 45 deg, %.3f at 85 deg\n", std::tan(pi / (2.0 * (2.0 / pi * (pi - std::asin(0.85))))) / 12.0 * 180.0 / pi, tire_lateral_ratio(pi / 4.0, 12.0, 0.85), tire_lateral_ratio(1.48, 12.0, 0.85));
}

// Creeping at about 2 m/s with the steering swung from side to side: the lateral speed follows the steering smoothly (the sum of its tick-to-tick changes stays near the sum of the steady swings), where a tire
// model that out-pushed the sideslip it is cancelling would ring at the tick rate.
double creep_roughness(bool slip_model, float stiffness = 12.0f) {
    const Ground g = ground(Surface::Asphalt, Surface::Asphalt);
    QueryScratch scratch;
    VehicleConfig cfg;
    cfg.powertrain.enabled = true;
    cfg.slip_angle_lateral = slip_model;
    cfg.lateral_stiffness = stiffness;
    Vehicle v(cfg, {-240, 0, 0.9f}, 0.0f);
    VehicleInput in;
    in.throttle = 0.12f;
    double rough = 0.0, last = 0.0;
    for (int i = 0; i < 60 * 8; ++i) {
        in.steer = (i / 45) % 2 == 0 ? 0.8f : -0.8f;
        v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
        const double lat = lateral_speed(v);
        if (i > 120) rough += std::fabs(lat - last);
        last = lat;
    }
    std::printf("creeping at %.1f m/s with the steering swung: lateral speed roughness %.2f (%s, stiffness %.0f)\n", static_cast<double>(v.state().forward_speed), rough, slip_model ? "slip-angle" : "original", static_cast<double>(stiffness));
    return rough;
}

void test_creep() {
    const double original = creep_roughness(false), soft = creep_roughness(true), stiff = creep_roughness(true, 60.0f);
    require(soft < 1.1 * original, "at creeping speed the slip-angle model is as smooth as the original");
    require(stiff < 1.1 * original, "even with the stiffest tire: the lateral force never exceeds what would cancel the sideslip, so it cannot ring");
}

struct Slide {
    double max_beta, yaw_total, speed_end, beta_end, rate_end;
    double held;  // seconds with the body slip above 12 degrees
};
// Body slip: the angle between where the car points and where it is going, deg (positive: the velocity is to the left of the heading).
double body_slip_deg(const Vehicle& v) {
    const Vec3& vel = v.state().velocity;
    const auto axes = v.axes();
    const double along = vel.x * axes[0].x + vel.y * axes[0].y + vel.z * axes[0].z;
    return std::atan2(static_cast<double>(lateral_speed(v)), std::fabs(along)) * 57.29578;
}

Slide drift_run(bool slip_model, bool recover) {
    const Ground g = ground(Surface::Asphalt, Surface::Asphalt);
    QueryScratch scratch;
    VehicleConfig cfg;
    cfg.powertrain.enabled = true;
    cfg.differential = VehicleDifferential::LimitedSlip;
    cfg.slip_angle_lateral = slip_model;
    Vehicle v(cfg, {-240, 0, 0.9f}, 0.0f);
    VehicleInput in;
    in.throttle = 1.0f;
    for (int i = 0; i < 60 * 60 && v.state().forward_speed < 22.0f; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    Slide out{};
    double unwrapped = 0.0, last_yaw = v.state().yaw;
    for (int i = 0; i < 60 * 6; ++i) {
        const double t = i / 60.0;
        in.throttle = 1.0f;
        in.steer = t < 2.0 ? 0.7f : 0.0f;
        if (recover && t >= 2.0) {
            in.throttle = 0.3f;
            in.steer = std::clamp(static_cast<float>(-body_slip_deg(v) * 0.04), -1.0f, 1.0f);  // steer into the slide
        }
        v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
        const double beta = body_slip_deg(v);
        out.max_beta = std::max(out.max_beta, std::fabs(beta));
        if (std::fabs(beta) > 12.0) out.held += 1.0 / 60.0;
        double dy = v.state().yaw - last_yaw;
        while (dy > 3.14159265) dy -= 6.2831853;
        while (dy < -3.14159265) dy += 6.2831853;
        unwrapped += dy;
        last_yaw = v.state().yaw;
        out.beta_end = beta;
    }
    out.yaw_total = unwrapped * 57.29578;
    out.speed_end = v.state().forward_speed;
    out.rate_end = v.state().angular_velocity.z;
    return out;
}

void test_drift() {
    // Steer into a corner at 22 m/s with full power, then catch it: a feedback steer into the slide and a lifted throttle.
    const Slide drift = drift_run(true, true);
    const Slide legacy = drift_run(false, true);
    std::printf("controlled drift: slip-angle model max body slip %.1f deg, %.2f s above 12 deg, heading change %.0f deg, ends at %.1f m/s, slip %.1f deg, yaw rate %.2f; original model max %.1f deg, %.2f s above 12\n", drift.max_beta,
                drift.held, drift.yaw_total, drift.speed_end, drift.beta_end, drift.rate_end, legacy.max_beta, legacy.held);
    require(drift.held > 1.5 && drift.max_beta > 20.0, "the tail stays out: more than 12 degrees of body slip for over a second and a half");
    require(drift.max_beta < 60.0, "without spinning: the body slip stays well below 90 degrees");
    require(drift.speed_end > 10.0, "and the car is still moving at speed afterwards");
    require(std::fabs(drift.beta_end) < 3.0 && std::fabs(drift.rate_end) < 0.2, "the car is caught: straight again, no yaw rate");
    require(std::fabs(drift.yaw_total) < 180.0, "having turned less than half a circle");
    require(legacy.max_beta < 8.0 && legacy.held < 0.05, "control: the original lateral model does not drift under the same inputs");
    const Slide uncaught = drift_run(true, false);
    require(uncaught.held > drift.held && std::fabs(uncaught.yaw_total) > 150.0, "control: uncorrected, the slide keeps going and the car turns around");
}

struct Skid {
    double lat_accel, speed, wobble, yaw_rate_end;
};
// A steady corner: reach `speed`, hold the steer and throttle level for 4 s, measure lateral acceleration over the last second.
Skid skidpad(bool slip_model, float steer, float throttle_hold, double speed) {
    const Ground g = ground(Surface::Asphalt, Surface::Asphalt);
    QueryScratch scratch;
    VehicleConfig cfg;
    cfg.powertrain.enabled = true;
    cfg.slip_angle_lateral = slip_model;
    Vehicle v(cfg, {-240, 0, 0.9f}, 0.0f);
    VehicleInput in;
    in.throttle = 1.0f;
    for (int i = 0; i < 60 * 60 && v.state().forward_speed < speed; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    in.steer = steer;
    in.throttle = throttle_hold;
    Skid out{};
    double lat = 0.0, rate_min = 1e9, rate_max = -1e9;
    Vec3 vlast = v.state().velocity;
    int n = 0;
    for (int i = 0; i < 60 * 4; ++i) {
        v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
        if (i >= 60 * 3) {
            const Vec3 vel = v.state().velocity;
            const double ax = (vel.x - vlast.x) * 60.0, ay = (vel.y - vlast.y) * 60.0;
            const auto axes = v.axes();
            lat += ax * axes[1].x + ay * axes[1].y;
            ++n;
            rate_min = std::min(rate_min, static_cast<double>(v.state().angular_velocity.z));
            rate_max = std::max(rate_max, static_cast<double>(v.state().angular_velocity.z));
        }
        vlast = v.state().velocity;
    }
    out.lat_accel = lat / n;
    out.speed = v.state().forward_speed;
    out.wobble = rate_max - rate_min;
    out.yaw_rate_end = v.state().angular_velocity.z;
    return out;
}

void test_cornering() {
    // Not power-limited, the slip-angle model corners as the original does: the same lateral acceleration to within 10% at three steering angles with the throttle released.
    for (const float steer : {0.1f, 0.25f, 0.5f}) {
        const Skid a = skidpad(false, steer, 0.0f, 18.0), b = skidpad(true, steer, 0.0f, 18.0);
        std::printf("coasting corner, steer %.2f: original %.2f m/s^2, slip-angle %.2f\n", static_cast<double>(steer), a.lat_accel, b.lat_accel);
        if (steer < 0.4f) {
            require(std::fabs(b.lat_accel - a.lat_accel) < 0.1 * a.lat_accel + 0.1, "off the throttle, short of the grip limit, the slip-angle model corners like the original");
        } else {
            // Past the limit the tires slide, and a sliding tire grips `lateral_slide_ratio` (0.85) of what a rolling one does.
            require(std::fabs(b.lat_accel / a.lat_accel - 0.85) < 0.06, "at the limit it holds the sliding grip: the slide ratio of the original's peak");
        }
    }
    // Under power the driven tires spend their grip on drive force: the corner is wider than the original model's (which gave all the grip to lateral force), and it holds a steady yaw rate (no wobble).
    const Skid orig = skidpad(false, 0.25f, 1.0f, 18.0), power = skidpad(true, 0.25f, 1.0f, 18.0);
    std::printf("corner under full power, steer 0.25: original %.2f m/s^2 (yaw rate %.2f), slip-angle %.2f (yaw rate %.2f, wobble %.3f)\n", orig.lat_accel, orig.yaw_rate_end, power.lat_accel, power.yaw_rate_end, power.wobble);
    require(power.lat_accel < 0.8 * orig.lat_accel, "under full power the slip-angle model leaves less lateral grip on the driven wheels");
    require(power.wobble < 0.1, "yet the corner is steady");
    // Straight and steady: a small steering pulse at 30 m/s dies away without oscillating.
    const Ground g = ground(Surface::Asphalt, Surface::Asphalt);
    QueryScratch scratch;
    VehicleConfig cfg;
    cfg.powertrain.enabled = true;
    cfg.slip_angle_lateral = true;
    Vehicle v(cfg, {-240, 0, 0.9f}, 0.0f);
    VehicleInput in;
    in.throttle = 1.0f;
    for (int i = 0; i < 60 * 60 && v.state().forward_speed < 30.0f; ++i) v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
    in.throttle = 0.15f;
    in.steer = 0.15f;
    drive(v, in, 0.3, g, scratch);
    in.steer = 0.0f;
    int crossings = 0;
    double last = 0.0;
    for (int i = 0; i < 60 * 4; ++i) {
        v.step(in, 1.0f / 60.0f, g.world, scratch, &g.surfaces);
        const double r = v.state().angular_velocity.z;
        if (i > 10 && last * r < 0.0 && std::fabs(r) > 0.002) ++crossings;
        last = r;
    }
    std::printf("steering pulse at 30 m/s: yaw rate after 4 s %.4f rad/s, %d sign changes, sideslip %.3f m/s\n", static_cast<double>(v.state().angular_velocity.z), crossings, static_cast<double>(lateral_speed(v)));
    require(std::fabs(v.state().angular_velocity.z) < 0.02f, "a steering pulse's yaw rate has died away after four seconds");
    require(crossings <= 2, "without a ringing oscillation");
    require(std::fabs(lateral_speed(v)) < 0.3f, "and the car is not sliding sideways");
}

} // namespace

int main() {
    try {
        test_validation();
        test_neutral_is_bit_identical();
        test_asymmetric_traction();
        test_engine_braking();
        test_tire_curve();
        test_creep();
        test_drift();
        test_cornering();
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
    std::cout << "vehicle drivetrain tests passed\n";
    return 0;
}
