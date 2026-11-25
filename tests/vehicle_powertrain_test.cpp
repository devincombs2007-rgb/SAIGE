// The powertrain: an engine torque curve, a gearbox and a final drive in place of the flat engine force. Off by default and bit-identical then; on, the car launches, shifts up through the gears with
// the engine speed in its range, reaches a plausible 0 to 100 km/h time, shifts down as it slows, reverses from a standstill, and reports the gear and the engine speed.
#include "saige/vehicle.hpp"

#include <algorithm>
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
struct Rig {
    StaticCollision world = StaticCollision(std::vector<Aabb>{{{-kHuge, -kHuge, -2}, {kHuge, kHuge, 0}}}, {});
    QueryScratch scratch;
};

VehicleConfig with_powertrain() {
    VehicleConfig c;
    c.powertrain.enabled = true;
    return c;
}

bool same_state(const VehicleState& a, const VehicleState& b) {
    return std::memcmp(&a.position, &b.position, sizeof(Vec3)) == 0 && std::memcmp(&a.velocity, &b.velocity, sizeof(Vec3)) == 0 && std::memcmp(&a.orientation, &b.orientation, sizeof(VehicleQuat)) == 0 &&
           std::memcmp(&a.angular_velocity, &b.angular_velocity, sizeof(Vec3)) == 0;
}

void test_validation_and_default() {
    const auto bad = [](VehicleConfig c) {
        try {
            validate_vehicle_config(c);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    VehicleConfig c = with_powertrain();
    require(!bad(c), "the default powertrain is valid");
    c.powertrain.gear_count = 7;
    require(bad(c), "more gears than ratios is rejected");
    c = with_powertrain();
    c.powertrain.upshift_rpm = c.powertrain.downshift_rpm;
    require(bad(c), "upshift at or below downshift is rejected");
    c = with_powertrain();
    c.powertrain.torque_curve[2][0] = c.powertrain.torque_curve[1][0];
    require(bad(c), "a torque curve whose rpm does not ascend is rejected");
    c = with_powertrain();
    c.powertrain.drivetrain_efficiency = 1.4f;
    require(bad(c), "an efficiency above 1 is rejected");
    c = with_powertrain();
    c.powertrain.gear_ratios[0] = -1.0f;
    require(bad(c), "a negative gear ratio is rejected");
    // Disabled: an invalid powertrain is not looked at, the car is the old one and reports no gear.
    VehicleConfig off;
    off.powertrain.gear_count = 99;
    require(!bad(off), "a disabled powertrain is not validated");
    Rig r;
    Vehicle a(VehicleConfig{}, {0, 0, 0.9f}, 0.0f), b(off, {0, 0, 0.9f}, 0.0f);
    bool same = true;
    for (int i = 0; i < 400; ++i) {
        VehicleInput in;
        in.throttle = (i % 150) < 100 ? 1.0f : -0.5f;
        in.steer = std::sin(static_cast<float>(i) * 0.03f);
        a.step(in, 1.0f / 60.0f, r.world, r.scratch);
        b.step(in, 1.0f / 60.0f, r.world, r.scratch);
        same = same && same_state(a.state(), b.state());
    }
    require(same, "with the powertrain disabled every state is bit-identical to the configuration without it");
    require(a.state().gear == 0 && a.state().engine_rpm == 0.0f, "no gear or engine speed without a powertrain");
}

void test_launch_and_shifts() {
    Rig r;
    Vehicle v(with_powertrain(), {-240, 0, 0.9f}, 0.0f);
    VehicleInput gas;
    gas.throttle = 1.0f;
    std::vector<int> gears;
    float t100 = -1.0f, t60 = -1.0f, rpm_max = 0.0f, rpm_min_moving = 1e9f;
    int shifts = 0, last_gear = 1;
    bool gear_never_skips = true;
    for (int i = 0; i < 60 * 25; ++i) {
        v.step(gas, 1.0f / 60.0f, r.world, r.scratch);
        const VehicleState& s = v.state();
        const float t = static_cast<float>(i + 1) / 60.0f;
        if (t60 < 0.0f && s.forward_speed > 60.0f / 3.6f) t60 = t;
        if (t100 < 0.0f && s.forward_speed > 100.0f / 3.6f) t100 = t;
        rpm_max = std::max(rpm_max, s.engine_rpm);
        if (s.forward_speed > 3.0f) rpm_min_moving = std::min(rpm_min_moving, s.engine_rpm);
        if (s.gear != last_gear) {
            gear_never_skips = gear_never_skips && s.gear == last_gear + 1;
            ++shifts;
            last_gear = s.gear;
        }
        if (s.position.x > 240.0f) break;
    }
    std::cout << "  0-60 km/h in " << t60 << " s, 0-100 km/h in " << t100 << " s; " << shifts << " upshifts to gear " << last_gear << "; engine speed " << rpm_min_moving << " to " << rpm_max << " rpm\n";
    require(t60 > 2.0f && t60 < 6.0f, "0 to 60 km/h in a plausible time for 134 kW and 1450 kg");
    require(t100 > 5.0f && t100 < 14.0f, "0 to 100 km/h in a plausible time");
    require(shifts >= 3 && gear_never_skips, "it shifts up through the gears one at a time");
    require(rpm_max <= 6800.0f * 1.05f + 1.0f, "the engine speed never goes far past the redline");
    require(rpm_min_moving >= 900.0f, "and never under the idle speed");
    // The same car with the flat engine force reaches a similar top-end acceleration order of magnitude (a sanity bound on the curve's scale): it is within a factor of 2.5 on 0 to 100.
    Vehicle flat(VehicleConfig{}, {-240, 0, 0.9f}, 0.0f);
    float flat100 = -1.0f;
    for (int i = 0; i < 60 * 25 && flat100 < 0.0f; ++i) {
        flat.step(gas, 1.0f / 60.0f, r.world, r.scratch);
        if (flat.state().forward_speed > 100.0f / 3.6f) flat100 = static_cast<float>(i + 1) / 60.0f;
    }
    require(flat100 > 0.0f && t100 < 2.5f * flat100 && flat100 < 2.5f * t100, "the powertrain's 0 to 100 is within a factor of 2.5 of the flat-force model's");
}

void test_efficiency_and_curve_scale_the_drive() {
    Rig r;
    const auto time_to_60 = [&](const VehicleConfig& cfg) {
        Vehicle v(cfg, {-240, 0, 0.9f}, 0.0f);
        VehicleInput gas;
        gas.throttle = 1.0f;
        for (int i = 0; i < 60 * 20; ++i) {
            v.step(gas, 1.0f / 60.0f, r.world, r.scratch);
            if (v.state().forward_speed > 60.0f / 3.6f) return static_cast<float>(i + 1) / 60.0f;
        }
        return 99.0f;
    };
    VehicleConfig base = with_powertrain(), lossy = with_powertrain(), weak = with_powertrain();
    lossy.powertrain.drivetrain_efficiency = 0.45f;
    for (auto& point : weak.powertrain.torque_curve) point[1] *= 0.5f;
    const float t = time_to_60(base), t_lossy = time_to_60(lossy), t_weak = time_to_60(weak);
    std::cout << "  0-60 km/h: " << t << " s; half the efficiency " << t_lossy << " s; half the torque " << t_weak << " s\n";
    require(t_lossy > 1.3f * t && t_weak > 1.3f * t, "less efficiency or less torque makes the launch slower");
    require(std::fabs(t_lossy - t_weak) < 0.15f * t_weak, "and halving either has the same effect (the force is their product)");
}

void test_downshift_and_reverse() {
    Rig r;
    Vehicle v(with_powertrain(), {-240, 0, 0.9f}, 0.0f);
    VehicleInput gas, coast_brake;
    gas.throttle = 1.0f;
    coast_brake.brake = 1.0f;
    for (int i = 0; i < 60 * 12 && v.state().forward_speed < 30.0f; ++i) v.step(gas, 1.0f / 60.0f, r.world, r.scratch);
    const int fast_gear = v.state().gear;
    require(fast_gear >= 3, "at 30 m/s it is in a high gear");
    bool down = false;
    for (int i = 0; i < 60 * 15 && v.state().forward_speed > 1.0f; ++i) {
        const int before = v.state().gear;
        v.step(coast_brake, 1.0f / 60.0f, r.world, r.scratch);
        down = down || v.state().gear < before;
    }
    require(down && v.state().gear <= 2, "braking to a stop shifts down");
    // Reverse from a standstill.
    VehicleInput rev;
    rev.throttle = -1.0f;
    for (int i = 0; i < 60 * 4; ++i) v.step(rev, 1.0f / 60.0f, r.world, r.scratch);
    std::cout << "  reverse: gear " << v.state().gear << ", speed " << v.state().forward_speed << " m/s\n";
    require(v.state().gear == -1 && v.state().forward_speed < -3.0f && v.state().forward_speed > -7.6f, "a negative throttle from a standstill reverses, up to the reverse speed limit");
    // Forward again: positive throttle while rolling backward brakes it, then it goes forward in first.
    for (int i = 0; i < 60 * 8; ++i) v.step(gas, 1.0f / 60.0f, r.world, r.scratch);
    require(v.state().forward_speed > 3.0f && v.state().gear >= 1, "positive throttle turns it round to forward");
}

void test_shifts_cut_the_torque_and_the_rev_limit_holds() {
    Rig r;
    Vehicle v(with_powertrain(), {-240, 0, 0.9f}, 0.0f);
    VehicleInput gas;
    gas.throttle = 1.0f;
    float before = 0.0f, during_min = 1e9f;
    float previous_speed = 0.0f;
    int previous_gear = 1, window = 0;
    bool saw_shift = false;
    for (int i = 0; i < 60 * 10 && !saw_shift; ++i) {
        v.step(gas, 1.0f / 60.0f, r.world, r.scratch);
        const float accel = (v.state().forward_speed - previous_speed) * 60.0f;
        if (v.state().gear != previous_gear && previous_gear >= 2) {
            saw_shift = true;
            window = 12;  // 0.2 s of the 0.25 s shift
        }
        if (!saw_shift && v.state().gear >= 2) before = accel;
        previous_speed = v.state().forward_speed;
        previous_gear = v.state().gear;
    }
    for (int i = 0; i < window; ++i) {
        v.step(gas, 1.0f / 60.0f, r.world, r.scratch);
        during_min = std::min(during_min, (v.state().forward_speed - previous_speed) * 60.0f);
        previous_speed = v.state().forward_speed;
    }
    std::cout << "  acceleration before a 2-to-3 shift " << before << " m/s^2, in it " << during_min << " m/s^2\n";
    require(saw_shift && before > 2.0f && during_min < 0.5f, "the engine delivers no torque during a shift: the acceleration drops and returns");
    // The rev limiter: in a single gear the car cannot pull past the speed of the redline.
    VehicleConfig one = with_powertrain();
    one.powertrain.gear_count = 1;
    one.powertrain.upshift_rpm = one.powertrain.redline_rpm;
    Vehicle w(one, {-240, 0, 0.9f}, 0.0f);
    float top = 0.0f;
    for (int i = 0; i < 60 * 14 && w.state().position.x < 240.0f; ++i) {
        w.step(gas, 1.0f / 60.0f, r.world, r.scratch);
        top = std::max(top, w.state().forward_speed);
    }
    const float redline_speed = 6800.0f * 2.0f * 3.14159265f / 60.0f * 0.33f / (3.6f * 3.7f);
    std::cout << "  first gear alone: top speed " << top << " m/s (the redline is at " << redline_speed << " m/s)\n";
    require(top > 0.85f * redline_speed && top < 1.04f * redline_speed, "the rev limiter holds the car at the redline speed of the gear");
}

void test_engine_speed_follows_the_wheels_and_the_clutch() {
    Rig r;
    Vehicle v(with_powertrain(), {0, 0, 0.9f}, 0.0f);
    VehicleInput idle;
    for (int i = 0; i < 30; ++i) v.step(idle, 1.0f / 60.0f, r.world, r.scratch);
    require(std::fabs(v.state().engine_rpm - 900.0f) < 1.0f, "at rest with no throttle the engine idles");
    VehicleInput gas;
    gas.throttle = 1.0f;
    v.step(gas, 1.0f / 60.0f, r.world, r.scratch);
    require(std::fabs(v.state().engine_rpm - 3000.0f) < 50.0f, "launching from a standstill the clutch slips and the engine is at the launch speed");
    // In gear, the engine speed is the wheel speed times the ratios (check at a steady 15 m/s in gear 2 by construction of the shift points).
    Vehicle u(with_powertrain(), {-240, 0, 0.9f}, 0.0f);
    float checked = 0.0f;
    for (int i = 0; i < 60 * 10 && checked == 0.0f; ++i) {
        u.step(gas, 1.0f / 60.0f, r.world, r.scratch);
        if (u.state().gear == 3 && u.state().forward_speed > 20.0f) {
            const double expected = static_cast<double>(u.state().forward_speed) / 0.33 * 1.5 * 3.7 * 60.0 / (2.0 * 3.14159265358979);
            require(std::fabs(static_cast<double>(u.state().engine_rpm) - expected) < 0.02 * expected, "in gear the engine speed is wheel speed times the gear and final drive");
            checked = 1.0f;
        }
    }
    require(checked > 0.0f, "reached third gear above 20 m/s");
}

} // namespace

int main() {
    try {
        test_validation_and_default();
        test_launch_and_shifts();
        test_efficiency_and_curve_scale_the_drive();
        test_downshift_and_reverse();
        test_shifts_cut_the_torque_and_the_rev_limit_holds();
        test_engine_speed_follows_the_wheels_and_the_clutch();
    } catch (const std::exception& e) {
        std::cerr << "vehicle powertrain test failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "vehicle powertrain tests passed\n";
}
