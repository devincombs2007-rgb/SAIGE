// The physics backend interface used by the evaluation prototype (and the shape of the abstraction SAIGE would put in front of a library). Only what the workloads need; deliberately small. A backend owns its
// world; bodies are small integer ids; all quantities are SI (meters, kilograms, seconds) in SAIGE axes (+X forward, +Y left, +Z up), gravity along -Z.
#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace eval {

struct V3 {
    double x = 0, y = 0, z = 0;
};
struct Q4 {
    double x = 0, y = 0, z = 0, w = 1;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline double length(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 rotate(Q4 q, V3 v) {  // q * v * q^-1
    const V3 u{q.x, q.y, q.z};
    const V3 t = V3{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x} * 2.0;
    const V3 c{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
    return v + t * q.w + c;
}
inline Q4 from_axis_angle(V3 axis, double angle) {
    const double l = length(axis), s = std::sin(angle * 0.5) / (l > 0 ? l : 1.0);
    return {axis.x * s, axis.y * s, axis.z * s, std::cos(angle * 0.5)};
}

struct Material {
    float friction = 0.6f;
    float restitution = 0.2f;
};

struct BoxDesc {
    V3 center;
    V3 half;
    double mass = 0.0;  // 0: static
    Material material;
    Q4 rotation;
    bool ccd = false;
};

class Backend {
public:
    virtual ~Backend() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    virtual int add_box(const BoxDesc& d) = 0;
    virtual int add_sphere(V3 center, double radius, double mass, Material m, bool ccd) = 0;
    // A swing-twist (ball with limits) joint between two bodies at a world pivot: the twist axis in world space, the swing cone half angle, the twist range.
    virtual void add_swing_twist(int a, int b, V3 pivot, V3 twist_axis, double swing_half_angle, double twist_min, double twist_max) = 0;
    virtual void add_hinge(int a, int b, V3 pivot, V3 axis, double min_angle, double max_angle) = 0;
    virtual void set_velocity(int id, V3 v) = 0;
    virtual void apply_force_at(int id, V3 force, V3 world_point) = 0;  // for one step
    [[nodiscard]] virtual V3 position(int id) const = 0;
    [[nodiscard]] virtual Q4 rotation(int id) const = 0;
    [[nodiscard]] virtual V3 velocity(int id) const = 0;
    [[nodiscard]] virtual V3 angular_velocity(int id) const = 0;
    [[nodiscard]] virtual bool active(int id) const = 0;
    [[nodiscard]] virtual int body_count() const = 0;
    virtual void step(double dt, int substeps) = 0;
    // True when this build can run the simulation on more than one thread; set_threads has no effect otherwise.
    [[nodiscard]] virtual bool multithreaded() const = 0;
    virtual void set_threads(int n) = 0;
    // Solver effort per step: velocity iterations (the default of each library is 10 for both).
    virtual void set_solver_iterations(int velocity_iterations) = 0;
    // A cast of a sphere along a segment against the world: the fraction of the segment at the first hit, or -1.
    [[nodiscard]] virtual double raycast_fraction(V3 from, V3 to) const = 0;
    // Switches the contact materials to SAIGE's rule (friction by the geometric mean, restitution by the larger) where the library's default differs.
    virtual void use_saige_material_rule() = 0;
};

std::unique_ptr<Backend> make_jolt(int max_bodies);
std::unique_ptr<Backend> make_bullet(int max_bodies);

} // namespace eval
