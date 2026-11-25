#include "saige/wheel_contact.hpp"

#include "saige/capsule_sweep.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

namespace saige {
namespace {

bool finite(Vec3 v) noexcept { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct Normalized {
    Vec3 direction;
    Vec3 up;
    double cos_slope;
};

Vec3 unit(Vec3 v, const char* what) {
    const double len = std::sqrt(static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y + static_cast<double>(v.z) * v.z);
    if (!(len > 0.0) || !std::isfinite(len)) throw std::invalid_argument(std::string("probe_wheel: ") + what + " must be finite and non-zero");
    return {static_cast<float>(v.x / len), static_cast<float>(v.y / len), static_cast<float>(v.z / len)};
}

Normalized validate(const WheelProbe& p) {
    if (!finite(p.origin) || !finite(p.direction) || !finite(p.up) || !std::isfinite(p.length) || !std::isfinite(p.radius) || !std::isfinite(p.max_slope_radians)) {
        throw std::invalid_argument("probe_wheel: the probe must be finite");
    }
    if (!(p.length > 0.0f) || p.length > max_sweep_length) throw std::invalid_argument("probe_wheel: the length must be in (0, max_sweep_length]");
    if (p.radius < 0.0f || p.radius > max_wheel_radius) throw std::invalid_argument("probe_wheel: the radius must be in [0, max_wheel_radius]");
    if (p.max_slope_radians < 0.0f || !(p.max_slope_radians < 1.5707963f)) throw std::invalid_argument("probe_wheel: the maximum slope must be in [0, pi/2)");
    return {unit(p.direction, "the direction"), unit(p.up, "up"), std::cos(static_cast<double>(p.max_slope_radians))};
}

double dot3(Vec3 a, Vec3 b) noexcept { return static_cast<double>(a.x) * b.x + static_cast<double>(a.y) * b.y + static_cast<double>(a.z) * b.z; }

WheelContact classify(WheelContact c, const Normalized& n, const WheelProbe& p) {
    c.slope_cos = static_cast<float>(dot3(c.normal, n.up));
    if (c.status != WheelContactStatus::InitialOverlap) c.status = dot3(c.normal, n.up) >= n.cos_slope ? WheelContactStatus::Support : WheelContactStatus::Blocked;
    c.travel = static_cast<float>(c.distance / static_cast<double>(p.length));
    c.center = {static_cast<float>(p.origin.x + n.direction.x * c.distance), static_cast<float>(p.origin.y + n.direction.y * c.distance),
                static_cast<float>(p.origin.z + n.direction.z * c.distance)};
    return c;
}

// A sphere is a capsule as tall as it is wide: feet at the bottom of the sphere.
Capsule sphere_of(const WheelProbe& p) noexcept { return Capsule{{p.origin.x, p.origin.y, p.origin.z - p.radius}, p.radius, 2.0f * p.radius}; }
Vec3 displacement_of(const WheelProbe& p, const Normalized& n) noexcept {
    return {n.direction.x * p.length, n.direction.y * p.length, n.direction.z * p.length};
}

// Everything that can make a probe throw, before anything is answered: its scalars and, for a sphere, the exact sweep envelope the capsule sweep will check.
Normalized prepare(const WheelProbe& p) {
    const Normalized n = validate(p);
    if (p.radius != 0.0f) check_sweep_envelope(sphere_of(p), displacement_of(p, n));
    return n;
}

WheelContact answer(const StaticCollision& world, const WheelProbe& p, const Normalized& n, QueryScratch& scratch) {
    WheelContact c;
    if (p.radius == 0.0f) {
        const auto hit = world.raycast(p.origin, n.direction, p.length, RayMode::Surface);
        if (!hit) return c;
        c.distance = hit->distance;
        c.point = hit->point;
        c.normal = hit->normal;
        c.primitive = hit->primitive;
        if (hit->started_inside) {
            c.status = WheelContactStatus::InitialOverlap;
            c.normal = {-n.direction.x, -n.direction.y, -n.direction.z};
            c.point = p.origin;
        }
        return classify(c, n, p);
    }
    const auto hit = world.sweep_capsule(sphere_of(p), displacement_of(p, n), scratch);
    if (!hit) return c;
    c.primitive = hit->primitive;
    c.normal = hit->hit.normal;
    c.point = hit->hit.point;
    if (hit->hit.started_overlapping) {
        c.status = WheelContactStatus::InitialOverlap;
        c.distance = 0.0;
    } else {
        c.distance = hit->hit.contact_t * static_cast<double>(p.length);
    }
    return classify(c, n, p);
}

} // namespace

WheelContact probe_wheel(const StaticCollision& world, const WheelProbe& probe) {
    QueryScratch scratch;
    return probe_wheel(world, probe, scratch);
}

WheelContact probe_wheel(const StaticCollision& world, const WheelProbe& probe, QueryScratch& scratch) {
    const Normalized n = prepare(probe);
    return answer(world, probe, n, scratch);
}

void probe_wheels(const StaticCollision& world, std::span<const WheelProbe> probes, std::span<WheelContact> out, QueryScratch& scratch) {
    if (probes.size() != out.size()) throw std::invalid_argument("probe_wheels: one output per probe");
    // Every probe is checked, to the exact sphere envelope, before any is answered: a bad one throws with `out` untouched. The second pass recomputes the
    // normalization rather than storing it, so the batch allocates nothing of its own.
    for (const WheelProbe& p : probes) static_cast<void>(prepare(p));
    for (std::size_t i = 0; i < probes.size(); ++i) out[i] = answer(world, probes[i], validate(probes[i]), scratch);
}

} // namespace saige
