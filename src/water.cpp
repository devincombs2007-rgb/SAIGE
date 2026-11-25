#include "saige/water.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige {
namespace {

constexpr double pi = 3.14159265358979323846;

struct V {
    double x{}, y{}, z{};
};
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(V a) { return std::sqrt(dot(a, a)); }

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
V qrotate(Q q, V v) {
    const V u{q.x, q.y, q.z};
    const V t = cross(u, v) * 2.0;
    return v + t * q.w + cross(u, t);
}
Q qconj(Q q) { return {-q.x, -q.y, -q.z, q.w}; }

bool finite(double x) { return std::isfinite(x); }
bool finite(WaterVec v) { return finite(v.x) && finite(v.y) && finite(v.z); }

// The dispersion relation: omega for wavenumber k in water of depth d (0 = deep).
double dispersion(double k, double depth, double g) { return depth > 0.0 ? std::sqrt(g * k * std::tanh(k * depth)) : std::sqrt(g * k); }

} // namespace

void validate_water_config(const WaterConfig& c) {
    if (!finite(c.level) || !finite(c.depth) || !finite(c.density) || !finite(c.gravity) || !finite(c.choppiness) || !finite(c.current)) throw std::invalid_argument("WaterConfig: a value is not finite");
    if (!(c.density > 0.0) || !(c.gravity > 0.0)) throw std::invalid_argument("WaterConfig: density and gravity must be positive");
    if (c.depth != 0.0 && !(c.depth >= 0.5)) throw std::invalid_argument("WaterConfig: a depth must be 0 (deep) or at least 0.5 m");
    if (!(c.choppiness >= 0.0 && c.choppiness <= 1.0)) throw std::invalid_argument("WaterConfig: choppiness must be in [0, 1]");
    if (c.waves.size() > 64) throw std::invalid_argument("WaterConfig: at most 64 waves");
    double steepness = 0.0;
    for (const WaterWave& w : c.waves) {
        if (!finite(w.direction) || !finite(w.wavelength) || !finite(w.amplitude) || !finite(w.phase)) throw std::invalid_argument("WaterConfig: a wave value is not finite");
        if (!(w.wavelength >= 0.5)) throw std::invalid_argument("WaterConfig: a wavelength under 0.5 m");
        if (!(w.amplitude >= 0.0) || w.amplitude > 0.1 * w.wavelength) throw std::invalid_argument("WaterConfig: an amplitude must be in [0, wavelength / 10]");
        steepness += (2.0 * pi / w.wavelength) * w.amplitude;
    }
    if (c.choppiness * steepness >= 0.95) throw std::invalid_argument("WaterConfig: choppiness times the sum of k*A must be below 0.95, or the surface folds over");
}

std::vector<WaterWave> waves_from_wind(double speed, double direction, int count, double depth, double gravity) {
    if (!(speed > 0.5) || !finite(speed) || !finite(direction) || count < 1 || count > 64 || !(gravity > 0.0)) throw std::invalid_argument("waves_from_wind: speed must be above 0.5 m/s, count 1 to 64");
    (void)depth;
    const double g = gravity;
    const double wp = 0.877 * g / speed;  // the peak angular frequency of a fully developed sea
    // Components spread from 0.6 to 3 times the peak frequency on a geometric grid; the energy of each is S(w) dw of the Pierson-Moskowitz spectrum, and its amplitude sqrt(2 S dw).
    const double lo = 0.6 * wp, hi = 3.0 * wp;
    const double ratio = std::pow(hi / lo, 1.0 / count);
    std::vector<WaterWave> out;
    double w0 = lo;
    for (int i = 0; i < count; ++i) {
        const double w1 = w0 * ratio, w = std::sqrt(w0 * w1), dw = w1 - w0;
        const double s = 0.0081 * g * g / std::pow(w, 5.0) * std::exp(-1.25 * std::pow(wp / w, 4.0));
        double amplitude = std::sqrt(2.0 * s * dw);
        const double k = w * w / g;  // deep-water wavenumber
        const double wavelength = 2.0 * pi / k;
        amplitude = std::min(amplitude, 0.09 * wavelength);
        // A fixed, well-spread direction offset and phase from the index (a golden-ratio sequence: no random source).
        const double u = std::fmod(0.61803398875 * (i + 1), 1.0), v = std::fmod(0.7548776662 * (i + 1), 1.0);
        out.push_back({direction + (u - 0.5) * 2.0 * (35.0 * pi / 180.0), wavelength, amplitude, v * 2.0 * pi});
        w0 = w1;
    }
    return out;
}

WaterSurface::WaterSurface(const WaterConfig& config) : config_(config) {
    validate_water_config(config_);
    for (const WaterWave& w : config_.waves) {
        const double k = 2.0 * pi / w.wavelength;
        k_.push_back(k);
        w_.push_back(dispersion(k, config_.depth, config_.gravity));
        cx_.push_back(std::cos(w.direction));
        cy_.push_back(std::sin(w.direction));
    }
}

void WaterSurface::source(double x, double y, double t, double& x0, double& y0) const {
    x0 = x;
    y0 = y;
    if (config_.choppiness == 0.0) return;
    for (int iteration = 0; iteration < 24; ++iteration) {
        double dx = 0.0, dy = 0.0;
        for (std::size_t i = 0; i < config_.waves.size(); ++i) {
            const double theta = k_[i] * (cx_[i] * x0 + cy_[i] * y0) - w_[i] * t + config_.waves[i].phase;
            const double s = config_.choppiness * config_.waves[i].amplitude * std::sin(theta);
            dx += cx_[i] * s;
            dy += cy_[i] * s;
        }
        // The displaced position of (x0, y0) is (x0 - dx, y0 - dy); we want it to be (x, y).
        const double nx = x + dx, ny = y + dy;
        const double change = std::fabs(nx - x0) + std::fabs(ny - y0);
        x0 = nx;
        y0 = ny;
        if (change < 1e-10) break;
    }
}

double WaterSurface::height(double x, double y, double t) const {
    double x0, y0;
    source(x, y, t, x0, y0);
    double h = config_.level;
    for (std::size_t i = 0; i < config_.waves.size(); ++i) h += config_.waves[i].amplitude * std::cos(k_[i] * (cx_[i] * x0 + cy_[i] * y0) - w_[i] * t + config_.waves[i].phase);
    return h;
}

WaterVec WaterSurface::normal(double x, double y, double t) const {
    // The surface (x(x0,y0), y(x0,y0), z(x0,y0)) has the tangents d/dx0 and d/dy0; the normal is their cross product.
    double x0, y0;
    source(x, y, t, x0, y0);
    double zx = 0.0, zy = 0.0, xx = 1.0, xy = 0.0, yx = 0.0, yy = 1.0;
    for (std::size_t i = 0; i < config_.waves.size(); ++i) {
        const double theta = k_[i] * (cx_[i] * x0 + cy_[i] * y0) - w_[i] * t + config_.waves[i].phase;
        const double a = config_.waves[i].amplitude, k = k_[i];
        const double s = std::sin(theta), c = std::cos(theta);
        zx += -a * k * cx_[i] * s;
        zy += -a * k * cy_[i] * s;
        // x = x0 - lambda * sum(d_x A sin), y = y0 - lambda * sum(d_y A sin)
        xx -= config_.choppiness * a * k * cx_[i] * cx_[i] * c;
        xy -= config_.choppiness * a * k * cx_[i] * cy_[i] * c;  // d x / d y0
        yx -= config_.choppiness * a * k * cy_[i] * cx_[i] * c;  // d y / d x0
        yy -= config_.choppiness * a * k * cy_[i] * cy_[i] * c;
    }
    const V tx{xx, yx, zx}, ty{xy, yy, zy};
    V n = cross(tx, ty);
    if (n.z < 0.0) n = n * -1.0;
    const double l = length(n);
    return l > 1e-12 ? WaterVec{n.x / l, n.y / l, n.z / l} : WaterVec{0, 0, 1};
}

double WaterSurface::jacobian(double x, double y, double t) const {
    double x0, y0;
    source(x, y, t, x0, y0);
    double xx = 1.0, xy = 0.0, yy = 1.0;
    for (std::size_t i = 0; i < config_.waves.size(); ++i) {
        const double theta = k_[i] * (cx_[i] * x0 + cy_[i] * y0) - w_[i] * t + config_.waves[i].phase;
        const double f = config_.choppiness * config_.waves[i].amplitude * k_[i] * std::cos(theta);
        xx -= f * cx_[i] * cx_[i];
        xy -= f * cx_[i] * cy_[i];
        yy -= f * cy_[i] * cy_[i];
    }
    return xx * yy - xy * xy;
}

double WaterSurface::foam(double x, double y, double t, double threshold) const {
    const double j = jacobian(x, y, t);
    return std::clamp((threshold - j) / threshold, 0.0, 1.0);
}

WaterVec WaterSurface::velocity(double x, double y, double z, double t) const {
    double x0, y0;
    source(x, y, t, x0, y0);
    WaterVec out = config_.current;
    for (std::size_t i = 0; i < config_.waves.size(); ++i) {
        const double a = config_.waves[i].amplitude, k = k_[i], w = w_[i];
        const double theta = k * (cx_[i] * x0 + cy_[i] * y0) - w * t + config_.waves[i].phase;
        // Below the mean level the orbit shrinks: exp(k z) in deep water, cosh/sinh of the depth in finite water; above it (a crest) the surface's own value.
        const double below = std::max(0.0, config_.level - z);
        double horizontal, vertical;
        if (config_.depth > 0.0) {
            const double d = config_.depth, zz = std::max(0.0, d - below);  // height above the bed
            horizontal = std::cosh(k * zz) / std::sinh(k * d);  // linear wave theory in finite depth: the orbits are ellipses, flatter toward the bed
            vertical = std::sinh(k * zz) / std::sinh(k * d);
        } else {
            horizontal = vertical = std::exp(-k * below);
        }
        // x = x0 - d A sin(theta) with theta = k.x0 - w t: dx/dt = d A w cos(theta), dz/dt = A w sin(theta) (the particle goes round its circle).
        out.x += cx_[i] * a * w * std::cos(theta) * horizontal;
        out.y += cy_[i] * a * w * std::cos(theta) * horizontal;
        out.z += a * w * std::sin(theta) * vertical;
    }
    return out;
}

FloatingBody::FloatingBody(const FloatingBodyConfig& config, WaterVec position) : config_(config) {
    const auto positive = [](double x) { return finite(x) && x > 0.0; };
    if (!finite(position) || !positive(config_.half_extents.x) || !positive(config_.half_extents.y) || !positive(config_.half_extents.z)) throw std::invalid_argument("FloatingBody: the extents must be finite and positive");
    if (!positive(config_.density) || !(config_.mass == 0.0 || positive(config_.mass))) throw std::invalid_argument("FloatingBody: density and mass must be positive");
    if (config_.cells < 2 || config_.cells > 16) throw std::invalid_argument("FloatingBody: cells must be 2 to 16");
    if (!finite(config_.center_of_mass) || std::fabs(config_.center_of_mass.x) > config_.half_extents.x || std::fabs(config_.center_of_mass.y) > config_.half_extents.y ||
        std::fabs(config_.center_of_mass.z) > config_.half_extents.z) {
        throw std::invalid_argument("FloatingBody: the center of mass must be inside the box");
    }
    if (!(config_.drag_speed_floor >= 0.0 && finite(config_.drag_speed_floor)) || !(config_.heave_damping >= 0.0 && config_.heave_damping <= 1.0) || !(config_.roll_damping >= 0.0 && config_.roll_damping <= 1.0)) throw std::invalid_argument("FloatingBody: the drag floor and the heave damping must be in range");
    if (!positive(config_.drag_coefficient) || !(config_.angular_drag >= 0.0 && finite(config_.angular_drag))) throw std::invalid_argument("FloatingBody: drag coefficients must be positive");
    const double hx = config_.half_extents.x, hy = config_.half_extents.y, hz = config_.half_extents.z;
    volume_ = 8.0 * hx * hy * hz;
    mass_ = config_.mass > 0.0 ? config_.mass : config_.density * volume_;
    // Box inertia about the box center, then the parallel axis to the center of mass.
    const double m = mass_;
    const V c{config_.center_of_mass.x, config_.center_of_mass.y, config_.center_of_mass.z};
    inertia_[0] = m / 3.0 * (hy * hy + hz * hz) + m * (c.y * c.y + c.z * c.z);
    inertia_[1] = m / 3.0 * (hx * hx + hz * hz) + m * (c.x * c.x + c.z * c.z);
    inertia_[2] = m / 3.0 * (hx * hx + hy * hy) + m * (c.x * c.x + c.y * c.y);
    com_[0] = c.x, com_[1] = c.y, com_[2] = c.z;
    const int n = config_.cells;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            for (int k = 0; k < n; ++k) {
                cells_.push_back({{(-1.0 + (2.0 * i + 1.0) / n) * hx, (-1.0 + (2.0 * j + 1.0) / n) * hy, (-1.0 + (2.0 * k + 1.0) / n) * hz}});
            }
        }
    }
    cell_volume_ = volume_ / static_cast<double>(cells_.size());
    cell_height_ = 2.0 * hz / n;
    // The mean of the three face areas shared out over the cells, so a fully submerged body's drag is the textbook one on a cube and a fair mean on a box.
    cell_area_ = (4.0 * hx * hy + 4.0 * hy * hz + 4.0 * hx * hz) / 3.0 / static_cast<double>(cells_.size());
    p_[0] = position.x, p_[1] = position.y, p_[2] = position.z;
    // The state's position is the box center; internally p_ is the center of mass.
    p_[0] += c.x, p_[1] += c.y, p_[2] += c.z;
    state_.position = position;
    state_.waterline = 0.0;
    state_.orientation = {0, 0, 0, 1};
}

void FloatingBody::set_velocity(WaterVec v) noexcept { v_[0] = v.x, v_[1] = v.y, v_[2] = v.z; state_.velocity = v; }

void FloatingBody::set_roll(double angle) noexcept {
    const Q before{q_[0], q_[1], q_[2], q_[3]};
    const V com_world_before = qrotate(before, V{com_[0], com_[1], com_[2]});
    const V center = V{p_[0], p_[1], p_[2]} - com_world_before;  // the box center
    const Q q = qnormalize(Q{std::sin(angle * 0.5), 0.0, 0.0, std::cos(angle * 0.5)});
    q_[0] = q.x, q_[1] = q.y, q_[2] = q.z, q_[3] = q.w;
    const V com_world = qrotate(q, V{com_[0], com_[1], com_[2]});
    const V p = center + com_world;
    p_[0] = p.x, p_[1] = p.y, p_[2] = p.z;
    state_.orientation = {q.x, q.y, q.z, q.w};
    state_.position = {center.x, center.y, center.z};
}

double FloatingBody::roll() const noexcept {
    const Q q{q_[0], q_[1], q_[2], q_[3]};
    const V up = qrotate(q, V{0, 0, 1});
    return std::atan2(-up.y, up.z);
}
double FloatingBody::pitch() const noexcept {
    const Q q{q_[0], q_[1], q_[2], q_[3]};
    const V up = qrotate(q, V{0, 0, 1});
    return std::atan2(up.x, up.z);
}

double FloatingBody::step(double dt, const WaterSurface& water, double t) {
    if (!finite(dt) || !(dt > 0.0) || dt > 1.0 || !finite(t)) throw std::invalid_argument("FloatingBody::step: dt must be finite, positive and at most one second");
    double remaining = dt;
    while (remaining > 1e-12) {
        const double slice = std::min(substep, remaining);
        substep_once(slice, water, t);
        t += slice;
        remaining -= slice;
    }
    return t;
}

void FloatingBody::substep_once(double dt, const WaterSurface& water, double t) {
    const WaterConfig& wc = water.config();
    Q q{q_[0], q_[1], q_[2], q_[3]};
    V p{p_[0], p_[1], p_[2]}, v{v_[0], v_[1], v_[2]}, w{w_[0], w_[1], w_[2]};
    const V com_world = qrotate(q, V{com_[0], com_[1], com_[2]});
    const V center = p - com_world;
    V force{0.0, 0.0, -mass_ * wc.gravity};
    V torque{};
    wet_.clear();
    double submerged = 0.0, surface_sum = 0.0;
    // The drag a cell may apply in one substep is limited so it can bring the cell's share of the body to rest relative to the water but never past it (the explicit integration stays stable at any speed).
    const double cell_mass = mass_ / static_cast<double>(cells_.size());
    for (const Cell& cell : cells_) {
        const V r = qrotate(q, V{cell.offset[0], cell.offset[1], cell.offset[2]});
        const V pos = center + r;
        const double surface = water.height(pos.x, pos.y, t);
        // Submerged fraction of the cell: its vertical extent against the surface (in a tilted body the cell is still a small block of the height of one layer).
        const double bottom = pos.z - 0.5 * cell_height_;
        const double s = std::clamp((surface - bottom) / cell_height_, 0.0, 1.0);
        if (s <= 0.0) continue;
        const V arm = pos - p;  // from the center of mass
        const V cell_velocity = v + cross(w, arm);
        const WaterVec flow = water.velocity(pos.x, pos.y, std::min(pos.z, surface), t);
        const V relative = cell_velocity - V{flow.x, flow.y, flow.z};
        const double speed = length(relative);
        V drag{};
        if (speed > 1e-12) {
            double coefficient = 0.5 * wc.density * config_.drag_coefficient * cell_area_ * s * (speed + config_.drag_speed_floor);
            coefficient = std::min(coefficient, 0.9 * cell_mass / dt);
            drag = relative * (-coefficient);
        }
        const V buoyancy{0.0, 0.0, wc.density * wc.gravity * cell_volume_ * s};
        force = force + buoyancy + drag;
        torque = torque + cross(arm, buoyancy) + cross(arm, drag) * config_.angular_drag;
        submerged += cell_volume_ * s;
        surface_sum += surface;
        wet_.push_back({{pos.x, pos.y, pos.z}, {cell_velocity.x, cell_velocity.y, cell_velocity.z}, s});
    }
    // Radiation damping of heave: a body that pierces the surface gives energy to the waves it makes as it bobs. Applied to the vertical velocity of the center of mass relative to the water's, with the
    // critical damping of the buoyancy stiffness of the waterplane (rho g A) and the mass.
    if (config_.heave_damping > 0.0 && !wet_.empty()) {
        bool piercing = false;
        for (const WetSample& cell : wet_) piercing = piercing || cell.submersion < 1.0;
        if (piercing) {
            const double stiffness = wc.density * wc.gravity * 4.0 * config_.half_extents.x * config_.half_extents.y;
            const double c = 2.0 * config_.heave_damping * std::sqrt(stiffness * mass_);
            const WaterVec flow = water.velocity(p.x, p.y, std::min(p.z, wc.level), t);
            force.z += -c * (v.z - flow.z);
        }
    }
    // Rotation: Euler's equations in the body frame.
    const Q qc = qconj(q);
    const V wb = qrotate(qc, w);
    V tb = qrotate(qc, torque);
    if (config_.roll_damping > 0.0 && !wet_.empty()) {
        bool piercing = false;
        for (const WetSample& cell : wet_) piercing = piercing || cell.submersion < 1.0;
        if (piercing) {
            // Wave-making damping of roll and pitch: critical damping of the waterplane's stiffness rho g I about each axis (I of the box's waterplane about that axis) and the body's inertia.
            const double ex = 2.0 * config_.half_extents.x, ey = 2.0 * config_.half_extents.y;
            const double k_roll = wc.density * wc.gravity * ex * ey * ey * ey / 12.0, k_pitch = wc.density * wc.gravity * ey * ex * ex * ex / 12.0;
            tb.x -= 2.0 * config_.roll_damping * std::sqrt(k_roll * inertia_[0]) * wb.x;
            tb.y -= 2.0 * config_.roll_damping * std::sqrt(k_pitch * inertia_[1]) * wb.y;
        }
    }
    const V gyro = cross(wb, V{inertia_[0] * wb.x, inertia_[1] * wb.y, inertia_[2] * wb.z});
    const V alpha_b{(tb.x - gyro.x) / inertia_[0], (tb.y - gyro.y) / inertia_[1], (tb.z - gyro.z) / inertia_[2]};
    v = v + force * (dt / mass_);
    w = w + qrotate(q, alpha_b) * dt;
    p = p + v * dt;
    const Q dq = qmul(Q{w.x * 0.5 * dt, w.y * 0.5 * dt, w.z * 0.5 * dt, 0.0}, q);
    q = qnormalize(Q{q.x + dq.x, q.y + dq.y, q.z + dq.z, q.w + dq.w});
    p_[0] = p.x, p_[1] = p.y, p_[2] = p.z;
    q_[0] = q.x, q_[1] = q.y, q_[2] = q.z, q_[3] = q.w;
    v_[0] = v.x, v_[1] = v.y, v_[2] = v.z;
    w_[0] = w.x, w_[1] = w.y, w_[2] = w.z;
    const V new_center = p - qrotate(q, V{com_[0], com_[1], com_[2]});
    state_.position = {new_center.x, new_center.y, new_center.z};
    state_.orientation = {q.x, q.y, q.z, q.w};
    state_.velocity = {v.x, v.y, v.z};
    state_.angular_velocity = {w.x, w.y, w.z};
    state_.submerged_volume = submerged;
    state_.waterline = wet_.empty() ? wc.level : surface_sum / static_cast<double>(wet_.size());
}

} // namespace saige
