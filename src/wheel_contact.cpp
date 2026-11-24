#include "saige/wheel_contact.hpp"

#include <cmath>
#include <stdexcept>
#include <vector>

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

bool origin_inside_a_box(const StaticCollision& world, Vec3 o) {
    std::vector<StaticPrimitive> near;
    world.query_region({o, o}, near);
    for (const StaticPrimitive& p : near) {
        if (p.kind != StaticPrimitive::Kind::Box) continue;
        const Aabb& b = world.boxes()[p.index];
        if (o.x > b.min.x && o.x < b.max.x && o.y > b.min.y && o.y < b.max.y && o.z > b.min.z && o.z < b.max.z) return true;
    }
    return false;
}

WheelContact classify(WheelContact c, const Normalized& n, const WheelProbe& p) {
    c.slope_cos = static_cast<float>(dot3(c.normal, n.up));
    if (c.status != WheelContactStatus::InitialOverlap) c.status = dot3(c.normal, n.up) >= n.cos_slope ? WheelContactStatus::Support : WheelContactStatus::Blocked;
    c.travel = static_cast<float>(c.distance / static_cast<double>(p.length));
    c.center = {static_cast<float>(p.origin.x + n.direction.x * c.distance), static_cast<float>(p.origin.y + n.direction.y * c.distance),
                static_cast<float>(p.origin.z + n.direction.z * c.distance)};
    return c;
}

WheelContact answer(const StaticCollision& world, const WheelProbe& p, const Normalized& n, QueryScratch& scratch) {
    WheelContact c;
    if (p.radius == 0.0f) {
        const auto hit = world.raycast(p.origin, n.direction, p.length);
        if (!hit) return c;
        c.distance = hit->distance;
        c.point = hit->point;
        c.normal = hit->normal;
        c.primitive = hit->primitive;
        if (hit->distance == 0.0 && origin_inside_a_box(world, p.origin)) {
            c.status = WheelContactStatus::InitialOverlap;
            c.normal = {-n.direction.x, -n.direction.y, -n.direction.z};
            c.point = p.origin;
        }
        return classify(c, n, p);
    }
    // A sphere is a capsule as tall as it is wide: feet at the bottom of the sphere.
    const Capsule sphere{{p.origin.x, p.origin.y, p.origin.z - p.radius}, p.radius, 2.0f * p.radius};
    const Vec3 displacement{n.direction.x * p.length, n.direction.y * p.length, n.direction.z * p.length};
    const auto hit = world.sweep_capsule(sphere, displacement, scratch);
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
    const Normalized n = validate(probe);
    return answer(world, probe, n, scratch);
}

void probe_wheels(const StaticCollision& world, std::span<const WheelProbe> probes, std::span<WheelContact> out, QueryScratch& scratch) {
    if (probes.size() != out.size()) throw std::invalid_argument("probe_wheels: one output per probe");
    std::vector<Normalized> normalized;
    normalized.reserve(probes.size());
    for (const WheelProbe& p : probes) normalized.push_back(validate(p));  // every probe is checked before any is answered
    for (std::size_t i = 0; i < probes.size(); ++i) out[i] = answer(world, probes[i], normalized[i], scratch);
}

} // namespace saige
