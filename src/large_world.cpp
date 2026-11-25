#include "saige/large_world.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace saige {
namespace {

void check_cell_size(double s) {
    if (!std::isfinite(s) || s <= 0.0 || s > 4096.0) throw std::invalid_argument("large world: the cell size must be finite, positive and at most 4096 m");
}

std::int32_t cell_index(double index) {
    if (!(index >= static_cast<double>(std::numeric_limits<std::int32_t>::min()) && index <= static_cast<double>(std::numeric_limits<std::int32_t>::max()))) throw std::out_of_range("large world: the position is outside the cell grid");
    return static_cast<std::int32_t>(index);
}

// One axis: the cell and the float offset in [0, s) of a double coordinate. The float rounding of an offset just under s would give s itself: that is the next cell's zero.
void split(double v, double s, std::int32_t& cell, float& local) {
    double c = std::floor(v / s);
    double l = v - c * s;
    if (l < 0.0) {  // v / s rounded up across a border
        c -= 1.0;
        l += s;
    }
    if (l >= s) {
        c += 1.0;
        l -= s;
    }
    float f = static_cast<float>(l);
    if (static_cast<double>(f) >= s) {
        c += 1.0;
        f = 0.0f;
    }
    cell = cell_index(c);
    local = f;
}

} // namespace

WorldPosition to_world_position(const DVec3& g, double s) {
    check_cell_size(s);
    if (!std::isfinite(g.x) || !std::isfinite(g.y) || !std::isfinite(g.z)) throw std::invalid_argument("large world: a coordinate is not finite");
    WorldPosition p;
    split(g.x, s, p.cell.x, p.local.x);
    split(g.y, s, p.cell.y, p.local.y);
    p.local.z = static_cast<float>(g.z);
    return p;
}

DVec3 to_global(const WorldPosition& p, double s) {
    check_cell_size(s);
    return {static_cast<double>(p.cell.x) * s + static_cast<double>(p.local.x), static_cast<double>(p.cell.y) * s + static_cast<double>(p.local.y), static_cast<double>(p.local.z)};
}

Vec3 relative_to(const WorldPosition& p, ChunkCoord frame, double s) {
    check_cell_size(s);
    const double dx = (static_cast<double>(p.cell.x) - static_cast<double>(frame.x)) * s + static_cast<double>(p.local.x);
    const double dy = (static_cast<double>(p.cell.y) - static_cast<double>(frame.y)) * s + static_cast<double>(p.local.y);
    return {static_cast<float>(dx), static_cast<float>(dy), p.local.z};
}

DVec3 difference(const WorldPosition& a, const WorldPosition& b, double s) {
    check_cell_size(s);
    return {(static_cast<double>(b.cell.x) - static_cast<double>(a.cell.x)) * s + (static_cast<double>(b.local.x) - static_cast<double>(a.local.x)),
            (static_cast<double>(b.cell.y) - static_cast<double>(a.cell.y)) * s + (static_cast<double>(b.local.y) - static_cast<double>(a.local.y)), static_cast<double>(b.local.z) - static_cast<double>(a.local.z)};
}

WorldPosition advance(const WorldPosition& p, const DVec3& d, double s) {
    check_cell_size(s);
    if (!std::isfinite(d.x) || !std::isfinite(d.y) || !std::isfinite(d.z)) throw std::invalid_argument("large world: a delta is not finite");
    WorldPosition out;
    const auto axis = [&](std::int32_t cell, float local, double delta, std::int32_t& oc, float& ol) {
        std::int32_t carry = 0;
        split(static_cast<double>(local) + delta, s, carry, ol);  // the whole cells the delta crosses and the normalized offset
        oc = cell_index(static_cast<double>(cell) + static_cast<double>(carry));
    };
    axis(p.cell.x, p.local.x, d.x, out.cell.x, out.local.x);
    axis(p.cell.y, p.local.y, d.y, out.cell.y, out.local.y);
    out.local.z = static_cast<float>(static_cast<double>(p.local.z) + d.z);
    return out;
}

Vec3 migration_shift(ChunkCoord from, ChunkCoord to, double s) {
    check_cell_size(s);
    return {static_cast<float>((static_cast<double>(from.x) - static_cast<double>(to.x)) * s), static_cast<float>((static_cast<double>(from.y) - static_cast<double>(to.y)) * s), 0.0f};
}

double float_resolution(double value) noexcept {
    const float f = static_cast<float>(std::fabs(value));
    return static_cast<double>(std::nextafter(f, std::numeric_limits<float>::infinity())) - static_cast<double>(f);
}

} // namespace saige
