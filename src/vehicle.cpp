#include "saige/vehicle.hpp"

#include "saige/wheel_contact.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige {
namespace {

struct D3 {
    double x{}, y{}, z{};
};
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(D3 a) { return std::sqrt(dot(a, a)); }
D3 normalized(D3 a, D3 fallback) {
    const double l = length(a);
    return l > 1e-12 ? a * (1.0 / l) : fallback;
}
D3 to_d3(Vec3 v) { return {static_cast<double>(v.x), static_cast<double>(v.y), static_cast<double>(v.z)}; }
Vec3 to_vec3(D3 v) { return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}; }

struct Q {
    double x{}, y{}, z{}, w{1.0};
};
Q qmul(Q a, Q b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q qnormalize(Q q) {
    const double l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-12 ? Q{q.x / l, q.y / l, q.z / l, q.w / l} : Q{};
}
D3 qrotate(Q q, D3 v) {
    const D3 u{q.x, q.y, q.z};
    const D3 t = cross(u, v) * 2.0;
    return v + t * q.w + cross(u, t);
}
Q qaxis(D3 axis, double angle) {
    const double s = std::sin(angle * 0.5);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5)};
}

bool finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
double clamp(double v, double lo, double hi) { return std::min(std::max(v, lo), hi); }
// A smooth sign: +-1 away from zero, linear within `band` of it, so a force that opposes a velocity does not chatter around rest.
double soft_sign(double v, double band) { return clamp(v / band, -1.0, 1.0); }

constexpr double gravity = 9.81;

} // namespace

void validate_vehicle_config(const VehicleConfig& c) {
    const auto positive = [](float v, const char* what) {
        if (!(v > 0.0f) || !std::isfinite(v)) throw std::invalid_argument(std::string("VehicleConfig: ") + what + " must be finite and positive");
    };
    positive(c.mass, "mass");
    positive(c.wheel_radius, "wheel_radius");
    positive(c.suspension_travel, "suspension_travel");
    positive(c.spring, "spring");
    positive(c.damper, "damper");
    positive(c.engine_force, "engine_force");
    positive(c.engine_power, "engine_power");
    positive(c.brake_force, "brake_force");
    positive(c.max_steer, "max_steer");
    positive(c.steer_speed_scale, "steer_speed_scale");
    positive(c.impact_event_speed, "impact_event_speed");
    positive(c.half_extents.x, "half_extents.x");
    positive(c.half_extents.y, "half_extents.y");
    positive(c.half_extents.z, "half_extents.z");
    if (!(c.tire_friction > 0.0f && c.tire_friction <= 3.0f)) throw std::invalid_argument("VehicleConfig: tire_friction must be in (0, 3]");
    if (!(c.tire_load_sensitivity >= 0.0f && c.tire_load_sensitivity <= 1.0f)) throw std::invalid_argument("VehicleConfig: tire_load_sensitivity must be in [0, 1]");
    if (!(c.handbrake_force >= 0.0f && std::isfinite(c.handbrake_force))) throw std::invalid_argument("VehicleConfig: handbrake_force must be finite and not negative");
    if (!(c.handbrake_grip > 0.0f && c.handbrake_grip <= 1.0f)) throw std::invalid_argument("VehicleConfig: handbrake_grip must be in (0, 1]");
    if (!(c.body_friction >= 0.0f && c.body_friction <= 2.0f)) throw std::invalid_argument("VehicleConfig: body_friction must be in [0, 2]");
    if (!(c.body_restitution >= 0.0f && c.body_restitution < 1.0f)) throw std::invalid_argument("VehicleConfig: body_restitution must be in [0, 1)");
    if (!(c.drag_area_cd >= 0.0f) || !std::isfinite(c.drag_area_cd) || !(c.rolling_resistance >= 0.0f) || !std::isfinite(c.rolling_resistance)) {
        throw std::invalid_argument("VehicleConfig: drag and rolling resistance must be finite and not negative");
    }
    if (c.powertrain.enabled) {
        const VehiclePowertrain& t = c.powertrain;
        const auto finite_positive = [](float x) { return std::isfinite(x) && x > 0.0f; };
        if (t.gear_count < 1 || t.gear_count > 6) throw std::invalid_argument("VehicleConfig: powertrain gear_count must be 1 to 6");
        for (int g = 0; g < t.gear_count; ++g) {
            if (!finite_positive(t.gear_ratios[static_cast<std::size_t>(g)])) throw std::invalid_argument("VehicleConfig: powertrain gear ratios must be finite and positive");
        }
        if (!finite_positive(t.reverse_ratio) || !finite_positive(t.final_drive) || !(t.drivetrain_efficiency > 0.0f && t.drivetrain_efficiency <= 1.0f)) {
            throw std::invalid_argument("VehicleConfig: powertrain reverse ratio, final drive and efficiency must be positive (efficiency at most 1)");
        }
        if (!finite_positive(t.reverse_speed_limit)) throw std::invalid_argument("VehicleConfig: powertrain reverse_speed_limit must be positive");
        if (!(finite_positive(t.idle_rpm) && t.launch_rpm >= t.idle_rpm && t.redline_rpm > t.launch_rpm && t.upshift_rpm > t.downshift_rpm && t.downshift_rpm > t.idle_rpm && t.upshift_rpm <= t.redline_rpm) ||
            !std::isfinite(t.launch_rpm) || !std::isfinite(t.redline_rpm) || !(t.shift_time >= 0.0f && std::isfinite(t.shift_time))) {
            throw std::invalid_argument("VehicleConfig: powertrain rpm limits must satisfy idle <= launch < redline and idle < downshift < upshift <= redline");
        }
        for (std::size_t k = 0; k < t.torque_curve.size(); ++k) {
            if (!std::isfinite(t.torque_curve[k][0]) || !std::isfinite(t.torque_curve[k][1]) || t.torque_curve[k][1] < 0.0f || (k > 0 && !(t.torque_curve[k][0] > t.torque_curve[k - 1][0]))) {
                throw std::invalid_argument("VehicleConfig: powertrain torque curve must have ascending rpm and non-negative torque");
            }
        }
    }
    if (c.max_steer >= 1.5f) throw std::invalid_argument("VehicleConfig: max_steer must be below 1.5 rad");
    if (!finite(c.center_of_mass) || std::fabs(c.center_of_mass.x) > c.half_extents.x || std::fabs(c.center_of_mass.y) > c.half_extents.y ||
        std::fabs(c.center_of_mass.z) > c.half_extents.z) {
        throw std::invalid_argument("VehicleConfig: the center of mass must be inside the chassis box");
    }
    int driven = 0;
    for (const VehicleWheelConfig& w : c.wheels) {
        if (!finite(w.mount) || std::fabs(w.mount.x) > c.half_extents.x + 1.0f || std::fabs(w.mount.y) > c.half_extents.y + 1.0f ||
            std::fabs(w.mount.z) > c.half_extents.z + c.suspension_travel) {
            throw std::invalid_argument("VehicleConfig: a wheel mount is outside the chassis' extent");
        }
        driven += w.driven ? 1 : 0;
    }
    if (driven == 0) throw std::invalid_argument("VehicleConfig: at least one wheel must be driven");
}

Vehicle::Vehicle(const VehicleConfig& config, Vec3 position, float yaw_radians) : config_(config) {
    validate_vehicle_config(config_);
    if (!finite(position) || !std::isfinite(yaw_radians)) throw std::invalid_argument("Vehicle: the position and heading must be finite");
    const Q q = qaxis({0.0, 0.0, 1.0}, static_cast<double>(yaw_radians));
    const D3 box_center = to_d3(position);
    const D3 com = box_center + qrotate(q, to_d3(config_.center_of_mass));
    body_.p[0] = com.x, body_.p[1] = com.y, body_.p[2] = com.z;
    body_.q[0] = q.x, body_.q[1] = q.y, body_.q[2] = q.z, body_.q[3] = q.w;
    // Principal moments of a solid box of the chassis' extent (the whole mass), about its center; the parallel-axis shift to the center of mass is small and omitted.
    const double m = static_cast<double>(config_.mass);
    const double hx = static_cast<double>(config_.half_extents.x), hy = static_cast<double>(config_.half_extents.y), hz = static_cast<double>(config_.half_extents.z);
    inertia_[0] = m / 3.0 * (hy * hy + hz * hz);
    inertia_[1] = m / 3.0 * (hx * hx + hz * hz);
    inertia_[2] = m / 3.0 * (hx * hx + hy * hy);
    publish();
}

OrientedBox Vehicle::chassis() const {
    const Q q{body_.q[0], body_.q[1], body_.q[2], body_.q[3]};
    const D3 com{body_.p[0], body_.p[1], body_.p[2]};
    const D3 center = com - qrotate(q, to_d3(config_.center_of_mass));
    return make_oriented_box(to_vec3(center), config_.half_extents, static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w));
}

std::array<Vec3, 3> Vehicle::axes() const noexcept {
    const Q q{body_.q[0], body_.q[1], body_.q[2], body_.q[3]};
    return {to_vec3(qrotate(q, {1, 0, 0})), to_vec3(qrotate(q, {0, 1, 0})), to_vec3(qrotate(q, {0, 0, 1}))};
}

double Vehicle::kinetic_energy() const noexcept {
    const D3 v{body_.v[0], body_.v[1], body_.v[2]}, w{body_.w[0], body_.w[1], body_.w[2]};
    const Q q{body_.q[0], body_.q[1], body_.q[2], body_.q[3]};
    // Rotational energy in the body frame.
    const Q qc{-q.x, -q.y, -q.z, q.w};
    const D3 wb = qrotate(qc, w);
    const double rot = 0.5 * (inertia_[0] * wb.x * wb.x + inertia_[1] * wb.y * wb.y + inertia_[2] * wb.z * wb.z);
    return 0.5 * static_cast<double>(config_.mass) * dot(v, v) + rot;
}

std::vector<VehicleImpact> Vehicle::take_impacts() {
    std::vector<VehicleImpact> out;
    out.swap(impacts_);
    return out;
}

void Vehicle::migrate_origin(Vec3 shift) noexcept {
    body_.p[0] += static_cast<double>(shift.x);
    body_.p[1] += static_cast<double>(shift.y);
    body_.p[2] += static_cast<double>(shift.z);
    publish();
}

void Vehicle::park() noexcept {
    for (int i = 0; i < 3; ++i) body_.v[i] = body_.w[i] = 0.0;
    publish();
}

void Vehicle::step(const VehicleInput& input, float dt, const StaticCollision& world, QueryScratch& scratch, const SurfaceMap* surfaces) {
    if (!(dt > 0.0f) || !std::isfinite(dt) || dt > 1.0f) throw std::invalid_argument("Vehicle::step: dt must be finite, positive and at most one second");
    if (!std::isfinite(input.throttle) || !std::isfinite(input.brake) || !std::isfinite(input.steer) || !std::isfinite(input.handbrake)) throw std::invalid_argument("Vehicle::step: the input must be finite");
    VehicleInput in = input;
    in.throttle = std::clamp(in.throttle, -1.0f, 1.0f);
    in.brake = std::clamp(in.brake, 0.0f, 1.0f);
    in.steer = std::clamp(in.steer, -1.0f, 1.0f);
    in.handbrake = std::clamp(in.handbrake, 0.0f, 1.0f);
    double remaining = static_cast<double>(dt);
    const double h = static_cast<double>(substep);
    while (remaining > 1e-9) {
        const double slice = std::min(h, remaining);
        substep_once(in, slice, world, scratch, surfaces);
        remaining -= slice;
    }
    publish();
}

void Vehicle::substep_once(const VehicleInput& input, double dt, const StaticCollision& world, QueryScratch& scratch, const SurfaceMap* surfaces) {
    ++substep_count_;
    const double mass = static_cast<double>(config_.mass);
    Q q{body_.q[0], body_.q[1], body_.q[2], body_.q[3]};
    D3 p{body_.p[0], body_.p[1], body_.p[2]}, v{body_.v[0], body_.v[1], body_.v[2]}, w{body_.w[0], body_.w[1], body_.w[2]};
    const D3 fwd = qrotate(q, {1, 0, 0}), left = qrotate(q, {0, 1, 0}), up = qrotate(q, {0, 0, 1});
    const D3 com_offset = qrotate(q, to_d3(config_.center_of_mass));
    const D3 box_center = p - com_offset;

    D3 force{0.0, 0.0, -mass * gravity};
    D3 torque{};
    const auto apply = [&](D3 f, D3 at) {
        force = force + f;
        torque = torque + cross(at - p, f);
    };

    int driven = 0;
    int rear_wheels = 0;
    for (const VehicleWheelConfig& wc : config_.wheels) {
        driven += wc.driven ? 1 : 0;
        rear_wheels += wc.steered ? 0 : 1;
    }
    const double speed_along = dot(v, fwd);
    const double steer_limit = static_cast<double>(config_.max_steer) / (1.0 + std::pow(std::fabs(speed_along) / static_cast<double>(config_.steer_speed_scale), 2.0));
    const double corner_mass = mass / 4.0;

    // The powertrain (when enabled): the total drive force at the wheels for this substep, from the engine speed the wheels and the gear give.
    const VehiclePowertrain& pt = config_.powertrain;
    double powertrain_force = 0.0;
    if (pt.enabled) {
        const double throttle = static_cast<double>(input.throttle);
        if (speed_along < 1.0 && throttle < 0.0) gear_ = -1;                                  // reverse from a standstill
        else if (gear_ == -1 && throttle > 0.0 && speed_along > -1.0) gear_ = 1;
        // A positive throttle while still rolling backward acts through first gear (it brakes the car and then drives it forward); the displayed gear changes once the car is no longer moving backward.
        const int g = (gear_ == -1 && throttle > 0.0) ? 1 : gear_;
        const double ratio = (g == -1 ? static_cast<double>(pt.reverse_ratio) : static_cast<double>(pt.gear_ratios[static_cast<std::size_t>(g - 1)])) * static_cast<double>(pt.final_drive);
        const double wheel_rpm = std::fabs(speed_along) / static_cast<double>(config_.wheel_radius) * ratio * 60.0 / (2.0 * 3.14159265358979323846);
        engine_rpm_ = std::max(wheel_rpm, static_cast<double>(throttle != 0.0 ? pt.launch_rpm : pt.idle_rpm));
        engine_rpm_ = std::min(engine_rpm_, static_cast<double>(pt.redline_rpm) * 1.05);
        if (shift_timer_ > 0.0) {
            shift_timer_ = std::max(0.0, shift_timer_ - dt);
        } else if (gear_ > 0 && g > 0) {
            if (throttle > 0.0 && wheel_rpm > static_cast<double>(pt.upshift_rpm) && gear_ < pt.gear_count) {
                ++gear_;
                shift_timer_ = static_cast<double>(pt.shift_time);
            } else if (wheel_rpm < static_cast<double>(pt.downshift_rpm) && gear_ > 1) {
                --gear_;
                shift_timer_ = static_cast<double>(pt.shift_time) * 0.5;
            }
        }
        double torque = 0.0;
        const auto& curve = pt.torque_curve;
        if (shift_timer_ <= 0.0 && engine_rpm_ < static_cast<double>(pt.redline_rpm) && !(g > 0 && throttle < 0.0 && speed_along > 1.0) && !(g == -1 && speed_along < -static_cast<double>(pt.reverse_speed_limit))) {
            const double rpm = engine_rpm_;
            if (rpm <= static_cast<double>(curve.front()[0])) {
                torque = static_cast<double>(curve.front()[1]);
            } else if (rpm >= static_cast<double>(curve.back()[0])) {
                torque = static_cast<double>(curve.back()[1]);
            } else {
                for (std::size_t k = 1; k < curve.size(); ++k) {
                    if (rpm <= static_cast<double>(curve[k][0])) {
                        const double t = (rpm - static_cast<double>(curve[k - 1][0])) / (static_cast<double>(curve[k][0]) - static_cast<double>(curve[k - 1][0]));
                        torque = static_cast<double>(curve[k - 1][1]) + (static_cast<double>(curve[k][1]) - static_cast<double>(curve[k - 1][1])) * t;
                        break;
                    }
                }
            }
        }
        powertrain_force = torque * ratio * static_cast<double>(pt.drivetrain_efficiency) / static_cast<double>(config_.wheel_radius) * std::fabs(throttle);
        if (g == -1) powertrain_force = -powertrain_force;
    }

    for (std::size_t i = 0; i < 4; ++i) {
        const VehicleWheelConfig& wc = config_.wheels[i];
        const double steer = wc.steered ? static_cast<double>(input.steer) * steer_limit : 0.0;
        const D3 mount = box_center + qrotate(q, to_d3(wc.mount));
        WheelProbe probe;
        probe.origin = to_vec3(mount);
        probe.direction = to_vec3(up * -1.0);
        probe.length = config_.suspension_travel;
        probe.radius = config_.wheel_radius;
        probe.up = to_vec3(up);
        probe.max_slope_radians = 1.2f;
        const WheelContact contact = probe_wheel(world, probe, scratch);
        steer_[i] = steer;
        const D3 steer_forward = qrotate(qaxis(up, steer), fwd);
        if (contact.status == WheelContactStatus::Miss) {
            compression_[i] = 0.0;
            // A wheel in the air keeps turning with the body's forward speed.
            spin_[i] += dot(v, steer_forward) / static_cast<double>(config_.wheel_radius) * dt;
            continue;
        }
        const double travel = static_cast<double>(config_.suspension_travel);
        const double distance = contact.status == WheelContactStatus::InitialOverlap ? 0.0 : contact.distance;
        const double compression = clamp(travel - distance, 0.0, travel);
        const double rate = (compression - compression_[i]) / dt;
        compression_[i] = compression;
        contact_point_[i] = contact.point;
        contact_normal_[i] = contact.normal;
        const double spring_force = std::max(0.0, static_cast<double>(config_.spring) * compression + static_cast<double>(config_.damper) * rate);
        apply(up * spring_force, mount);

        // The tire: forces in the contact plane, bounded by the friction circle of the load.
        const D3 n = normalized(to_d3(contact.normal), up);
        const D3 cp = to_d3(contact.point);
        const D3 v_cp = v + cross(w, cp - p);
        const D3 f = normalized(steer_forward - n * dot(steer_forward, n), fwd);
        const D3 l = normalized(cross(n, f), left);
        const double v_long = dot(v_cp, f), v_lat = dot(v_cp, l);
        double fx = 0.0;
        if (wc.driven && pt.enabled) {
            fx += powertrain_force / driven;
        } else if (wc.driven) {
            const double throttle = static_cast<double>(input.throttle);
            const double per_wheel = static_cast<double>(config_.engine_force) / driven;
            const double power_cap = static_cast<double>(config_.engine_power) / (std::max(std::fabs(v_long), 2.0) * driven);
            double drive = std::min(per_wheel, power_cap) * throttle;
            if (throttle < 0.0) drive *= 0.55;  // reverse is weaker
            fx += drive;
        }
        // What the tire is on: its grip scales the friction circle, its rolling resistance the rolling term (both 1 without a surface map).
        const PhysicsMaterial& surface = surfaces != nullptr ? surfaces->material_at(contact.primitive) : physics_material(Surface::Default);
        // The handbrake acts on the non-steered wheels: more braking force, and less of their grip. At 0 both terms are exactly zero / one, so the original arithmetic is unchanged.
        const double handbrake = wc.steered ? 0.0 : static_cast<double>(input.handbrake);
        fx -= soft_sign(v_long, 0.35) * (static_cast<double>(input.brake) * static_cast<double>(config_.brake_force) / 4.0 +
                                          static_cast<double>(config_.rolling_resistance) * static_cast<double>(surface.rolling_resistance) / 4.0 +
                                          handbrake * static_cast<double>(config_.handbrake_force) / static_cast<double>(std::max(1, rear_wheels)));
        double fy = -corner_mass * v_lat / dt * 0.55;
        double mu = static_cast<double>(config_.tire_friction);
        if (config_.tire_load_sensitivity > 0.0f) {  // grip falls with load above the static share of the weight
            const double static_load = mass * 9.81 / 4.0;
            mu *= clamp(1.0 - static_cast<double>(config_.tire_load_sensitivity) * (spring_force / static_load - 1.0), 0.5, 1.5);
        }
        double grip = mu * static_cast<double>(surface.tire_grip);
        if (handbrake > 0.0) grip *= 1.0 - handbrake * (1.0 - static_cast<double>(config_.handbrake_grip));
        const double limit = grip * spring_force;
        const double magnitude = std::hypot(fx, fy);
        if (magnitude > limit && magnitude > 0.0) {
            fx *= limit / magnitude;
            fy *= limit / magnitude;
        }
        apply(f * fx + l * fy, cp);
        spin_[i] += v_long / static_cast<double>(config_.wheel_radius) * dt;
    }

    // Aerodynamic drag on the body.
    const double speed = length(v);
    force = force - v * (static_cast<double>(config_.drag_area_cd) * speed);
    // A hint of angular damping keeps an unloaded body from spinning forever.
    torque = torque - w * (0.02 * mass);

    // Integrate (semi-implicit): velocities first, then pose.
    const Q qc{-q.x, -q.y, -q.z, q.w};
    const D3 wb = qrotate(qc, w);
    const D3 tb = qrotate(qc, torque);
    const D3 gyro = cross(wb, D3{inertia_[0] * wb.x, inertia_[1] * wb.y, inertia_[2] * wb.z});
    const D3 alpha_b{(tb.x - gyro.x) / inertia_[0], (tb.y - gyro.y) / inertia_[1], (tb.z - gyro.z) / inertia_[2]};
    v = v + force * (dt / mass);
    w = w + qrotate(q, alpha_b) * dt;
    p = p + v * dt;
    const Q dq = qmul(Q{w.x * 0.5 * dt, w.y * 0.5 * dt, w.z * 0.5 * dt, 0.0}, q);
    q = qnormalize(Q{q.x + dq.x, q.y + dq.y, q.z + dq.z, q.w + dq.w});

    // Collision: the chassis box against the snapshot, a positional correction and an impulse per penetrating primitive (up to the four deepest).
    {
        const D3 center = p - qrotate(q, to_d3(config_.center_of_mass));
        const OrientedBox box = make_oriented_box(to_vec3(center), config_.half_extents, static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w));
        overlaps_.clear();
        world.overlaps_oriented_box(box, overlaps_, scratch);
        std::sort(overlaps_.begin(), overlaps_.end(), [](const StaticOrientedBoxOverlap& a, const StaticOrientedBoxOverlap& b) { return a.overlap.depth > b.overlap.depth; });
        const std::size_t count = std::min<std::size_t>(overlaps_.size(), 4);
        const double ke_before = 0.5 * mass * dot(v, v);
        for (std::size_t k = 0; k < count; ++k) {
            const D3 n = normalized(to_d3(overlaps_[k].overlap.normal), D3{0, 0, 1});
            const double depth = static_cast<double>(overlaps_[k].overlap.depth);
            p = p + n * (depth * (k == 0 ? 0.85 : 0.4));
            // The contact point: the average of the box's lowest vertices against the normal (those within 6 cm of the deepest). For a primitive that is a box the
            // vertices must also lie inside it (grown by 3 cm), so a barrier that ends mid-way along the chassis' face is hit at the end that overlaps it, not at the
            // face's centre. A face-on hit gives the face centre (no invented spin); a corner hit gives the corner.
            const D3 ax = qrotate(q, {1, 0, 0}), ay = qrotate(q, {0, 1, 0}), az = qrotate(q, {0, 0, 1});
            const D3 box_c = p - qrotate(q, to_d3(config_.center_of_mass));
            std::array<D3, 8> corners;
            std::array<double, 8> depth_along{};
            double lowest = 1e30;
            for (int c = 0; c < 8; ++c) {
                const double sx = (c & 1) ? 1.0 : -1.0, sy = (c & 2) ? 1.0 : -1.0, sz = (c & 4) ? 1.0 : -1.0;
                corners[static_cast<std::size_t>(c)] = box_c + ax * (sx * static_cast<double>(config_.half_extents.x)) + ay * (sy * static_cast<double>(config_.half_extents.y)) +
                                                       az * (sz * static_cast<double>(config_.half_extents.z));
                depth_along[static_cast<std::size_t>(c)] = dot(corners[static_cast<std::size_t>(c)] - box_c, n);
                lowest = std::min(lowest, depth_along[static_cast<std::size_t>(c)]);
            }
            const StaticPrimitive prim = overlaps_[k].primitive;
            const auto inside_primitive = [&](D3 pt) {
                if (prim.kind != StaticPrimitive::Kind::Box) return true;
                const Aabb& b = world.boxes()[prim.index];
                const double m = 0.03;
                return pt.x >= static_cast<double>(b.min.x) - m && pt.x <= static_cast<double>(b.max.x) + m && pt.y >= static_cast<double>(b.min.y) - m &&
                       pt.y <= static_cast<double>(b.max.y) + m && pt.z >= static_cast<double>(b.min.z) - m && pt.z <= static_cast<double>(b.max.z) + m;
            };
            D3 sum{}, sum_all{};
            int count_in = 0, count_all = 0;
            for (std::size_t c = 0; c < 8; ++c) {
                if (depth_along[c] > lowest + 0.06) continue;
                sum_all = sum_all + corners[c];
                ++count_all;
                if (inside_primitive(corners[c])) {
                    sum = sum + corners[c];
                    ++count_in;
                }
            }
            const D3 point = count_in > 0 ? sum * (1.0 / count_in) : sum_all * (1.0 / std::max(count_all, 1));
            const D3 r = point - p;
            const D3 vp = v + cross(w, r);
            const double vn = dot(vp, n);
            if (vn >= 0.0) continue;  // separating
            // World-space inverse inertia applied to a vector: R diag(1/I) R^T.
            const auto inv_inertia = [&](D3 a) {
                const Q qi{-q.x, -q.y, -q.z, q.w};
                const D3 b = qrotate(qi, a);
                return qrotate(q, D3{b.x / inertia_[0], b.y / inertia_[1], b.z / inertia_[2]});
            };
            // What the body hits: with a surface map its restitution and friction combine with the primitive's (friction by the geometric mean, restitution by the larger); without one, the body's own.
            double e = static_cast<double>(config_.body_restitution), mu_body = static_cast<double>(config_.body_friction);
            if (surfaces != nullptr) {
                const PhysicsMaterial& hit_material = surfaces->material_at(prim);
                e = static_cast<double>(combine_restitution(config_.body_restitution, hit_material.restitution));
                mu_body = static_cast<double>(combine_friction(config_.body_friction, hit_material.friction));
            }
            const D3 rn = cross(r, n);
            const double denom = 1.0 / mass + dot(cross(inv_inertia(rn), r), n);
            const double jn = -(1.0 + e) * vn / denom;
            v = v + n * (jn / mass);
            w = w + inv_inertia(cross(r, n * jn));
            // Coulomb friction along the tangential relative velocity.
            const D3 vp2 = v + cross(w, r);
            D3 tangent = vp2 - n * dot(vp2, n);
            const double tl = length(tangent);
            if (tl > 1e-6) {
                tangent = tangent * (1.0 / tl);
                const double denom_t = 1.0 / mass + dot(cross(inv_inertia(cross(r, tangent)), r), tangent);
                const double jt = clamp(-tl / denom_t, -mu_body * jn, mu_body * jn);
                v = v + tangent * (jt / mass);
                w = w + inv_inertia(cross(r, tangent * jt));
            }
            if (-vn >= static_cast<double>(config_.impact_event_speed)) {
                VehicleImpact event;
                event.substep = substep_count_;
                event.speed = static_cast<float>(-vn);
                event.impulse = static_cast<float>(jn);
                event.point = to_vec3(point);
                event.normal = to_vec3(n);
                event.primitive = overlaps_[k].primitive;
                event.energy = static_cast<float>(std::max(0.0, ke_before - 0.5 * mass * dot(v, v)));
                impacts_.push_back(event);
            }
        }
    }

    body_.p[0] = p.x, body_.p[1] = p.y, body_.p[2] = p.z;
    body_.q[0] = q.x, body_.q[1] = q.y, body_.q[2] = q.z, body_.q[3] = q.w;
    body_.v[0] = v.x, body_.v[1] = v.y, body_.v[2] = v.z;
    body_.w[0] = w.x, body_.w[1] = w.y, body_.w[2] = w.z;
}

void Vehicle::publish() noexcept {
    const Q q{body_.q[0], body_.q[1], body_.q[2], body_.q[3]};
    const D3 p{body_.p[0], body_.p[1], body_.p[2]};
    const D3 center = p - qrotate(q, to_d3(config_.center_of_mass));
    state_.position = to_vec3(center);
    state_.orientation = {static_cast<float>(q.x), static_cast<float>(q.y), static_cast<float>(q.z), static_cast<float>(q.w)};
    state_.velocity = {static_cast<float>(body_.v[0]), static_cast<float>(body_.v[1]), static_cast<float>(body_.v[2])};
    state_.angular_velocity = {static_cast<float>(body_.w[0]), static_cast<float>(body_.w[1]), static_cast<float>(body_.w[2])};
    const D3 fwd = qrotate(q, {1, 0, 0});
    state_.forward_speed = static_cast<float>(body_.v[0] * fwd.x + body_.v[1] * fwd.y + body_.v[2] * fwd.z);
    state_.yaw = static_cast<float>(std::atan2(fwd.y, fwd.x));
    state_.wheels_in_contact = 0;
    state_.gear = config_.powertrain.enabled ? gear_ : 0;
    state_.engine_rpm = config_.powertrain.enabled ? static_cast<float>(engine_rpm_) : 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
        VehicleWheelState& ws = state_.wheels[i];
        ws.in_contact = compression_[i] > 0.0;
        ws.compression = static_cast<float>(compression_[i]);
        ws.spin = static_cast<float>(std::remainder(spin_[i], 2.0 * 3.14159265358979323846));
        ws.load = static_cast<float>(static_cast<double>(config_.spring) * compression_[i]);
        ws.steer_angle = static_cast<float>(steer_[i]);
        ws.contact_point = contact_point_[i];
        ws.contact_normal = contact_normal_[i];
        state_.wheels_in_contact += ws.in_contact ? 1 : 0;
    }
}

} // namespace saige
