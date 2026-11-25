#include "saige/buoyancy.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige {
namespace {

using V = DVec;
V operator+(V a, V b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V operator-(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V operator*(V a, double s) { return {a.x * s, a.y * s, a.z * s}; }
V cross(V a, V b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double length(V a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V rotate(const DQuat& q, V v) {
    const V u{q.x, q.y, q.z};
    const V t = cross(u, v) * 2.0;
    return v + t * q.w + cross(u, t);
}
V rotate_inverse(const DQuat& q, V v) { return rotate(DQuat{-q.x, -q.y, -q.z, q.w}, v); }

} // namespace

BuoyancyResult apply_buoyancy(DynamicsWorld& world, BodyId body, const BuoyantBox& b, const WaterSource& water, double t, double dt) {
    const bool finite = std::isfinite(b.half_extents.x + b.half_extents.y + b.half_extents.z + b.mass + b.drag_coefficient + b.angular_drag + b.drag_speed_floor + b.heave_damping + b.roll_damping + t + dt);
    if (!finite || !(b.half_extents.x > 0.0 && b.half_extents.y > 0.0 && b.half_extents.z > 0.0) || !(b.mass > 0.0) || b.cells < 2 || b.cells > 16 || !(dt > 0.0) || b.drag_coefficient < 0.0 || b.angular_drag < 0.0 || b.drag_speed_floor < 0.0 ||
        b.heave_damping < 0.0 || b.roll_damping < 0.0) {
        throw std::invalid_argument("apply_buoyancy: a size, the mass or dt is not positive and finite, or a coefficient is negative, or the cell count is out of 2 to 16");
    }
    const WaterConfig& wc = water.config();
    const DBodyState s = world.state(body);
    const int n = b.cells;
    const double hx = b.half_extents.x, hy = b.half_extents.y, hz = b.half_extents.z;
    const double volume = 8.0 * hx * hy * hz, cells = static_cast<double>(n) * n * n;
    const double cell_volume = volume / cells, cell_height = 2.0 * hz / n, cell_mass = b.mass / cells;
    const double cell_area = (4.0 * hx * hy + 4.0 * hy * hz + 4.0 * hx * hz) / 3.0 / cells;
    BuoyancyResult out;
    out.waterline = wc.level;
    double surface_sum = 0.0;
    V force_total{}, torque_total{};
    bool piercing = false;
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            for (int k = 0; k < n; ++k) {
                const V r = rotate(s.rotation, V{(-1.0 + (2.0 * i + 1.0) / n) * hx, (-1.0 + (2.0 * j + 1.0) / n) * hy, (-1.0 + (2.0 * k + 1.0) / n) * hz});
                const V pos = s.position + r;
                double surface;
                WaterVec flow;
                water.sample(pos.x, pos.y, pos.z, t, surface, flow, pos.z - 0.5 * cell_height);
                const double submerged = std::clamp((surface - (pos.z - 0.5 * cell_height)) / cell_height, 0.0, 1.0);
                if (submerged <= 0.0) continue;
                if (submerged < 1.0) piercing = true;
                const V velocity = s.linear_velocity + cross(s.angular_velocity, r);
                const V relative = velocity - V{flow.x, flow.y, flow.z};
                const double speed = length(relative);
                V drag{};
                if (speed > 1e-12) {
                    double coefficient = 0.5 * wc.density * b.drag_coefficient * cell_area * submerged * (speed + b.drag_speed_floor);
                    coefficient = std::min(coefficient, 0.9 * cell_mass / dt);  // never past rest relative to the water in one step
                    drag = relative * (-coefficient);
                }
                const V buoyancy{0.0, 0.0, wc.density * wc.gravity * cell_volume * submerged};
                const V f = buoyancy + drag;
                // The drag's torque is scaled (angular_drag), the buoyancy's is exact: apply the buoyancy at the cell and the drag as a force at the centre plus the scaled couple.
                world.add_impulse(body, buoyancy * dt, pos);
                world.add_impulse(body, drag * dt, s.position);
                const V couple = cross(r, drag) * b.angular_drag;
                force_total = force_total + f;
                torque_total = torque_total + couple;
                out.submerged_volume += cell_volume * submerged;
                surface_sum += surface;
                ++out.wet_cells;
            }
        }
    }
    if (out.wet_cells > 0) {
        out.waterline = surface_sum / static_cast<double>(out.wet_cells);
        world.add_angular_impulse(body, torque_total * dt);
        if (piercing) {
            if (b.heave_damping > 0.0) {
                const double stiffness = wc.density * wc.gravity * 4.0 * hx * hy;
                const double c = 2.0 * b.heave_damping * std::sqrt(stiffness * b.mass);
                double surface_here;
                WaterVec flow;
                water.sample(s.position.x, s.position.y, std::min(s.position.z, wc.level), t, surface_here, flow);
                world.add_impulse(body, V{0.0, 0.0, -c * (s.linear_velocity.z - flow.z) * dt}, s.position);
            }
            if (b.roll_damping > 0.0) {
                // Critical damping of the waterplane's stiffness about the body's x and y axes, with the box's inertia about them.
                const double ex = 2.0 * hx, ey = 2.0 * hy;
                const double ix = b.mass * (4.0 * hy * hy + 4.0 * hz * hz) / 12.0, iy = b.mass * (4.0 * hx * hx + 4.0 * hz * hz) / 12.0;
                const double k_roll = wc.density * wc.gravity * ex * ey * ey * ey / 12.0, k_pitch = wc.density * wc.gravity * ey * ex * ex * ex / 12.0;
                const V wb = rotate_inverse(s.rotation, s.angular_velocity);
                const V tb{-2.0 * b.roll_damping * std::sqrt(k_roll * ix) * wb.x, -2.0 * b.roll_damping * std::sqrt(k_pitch * iy) * wb.y, 0.0};
                world.add_angular_impulse(body, rotate(s.rotation, tb) * dt);
            }
        }
    }
    return out;
}

} // namespace saige
