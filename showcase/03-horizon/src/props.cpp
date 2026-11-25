#include "props.hpp"

#include <cmath>
#include <stdexcept>

namespace saige::horizon {

namespace {
constexpr float half_pi = 1.57079632679489662f;

// Authored bounds are what `saige_asset_inspect` reports for the Poly Haven models (engine space, importer conversion applied). horizon_props_test checks this table against the real files
// (through the engine's own importer) whenever they are present, so a changed asset or a typo fails a test instead of shipping.
const PropSpec table[] = {
    {PropType::Barrier, "concrete_road_barrier_02", -half_pi, 1.0f, {-0.254f, -0.776f, -0.003f, 0.188f, 0.789f, 1.109f}, true, {-0.254f, -0.776f, -0.003f, 0.188f, 0.789f, 1.109f}},
    {PropType::Lamp, "street_lamp_01", 0.0f, 1.5f, {-0.193f, -0.352f, 0.0f, 0.193f, 0.352f, 3.871f}, true, {-0.15f, -0.15f, 0.0f, 0.15f, 0.15f, 3.871f}},  // the pole, not the arm
    {PropType::UtilityBox, "utility_box_01", 0.0f, 1.0f, {-0.210f, -0.260f, 0.0f, 0.222f, 0.260f, 1.120f}, true, {-0.210f, -0.260f, 0.0f, 0.222f, 0.260f, 1.120f}},
    {PropType::RockLarge, "rock_07", 0.0f, 0.7f, {-0.178f, -0.088f, -0.002f, 0.142f, 0.081f, 0.142f}, true, {-0.142f, -0.070f, 0.0f, 0.114f, 0.065f, 0.118f}},  // a box inset by a fifth: a rock is a lump, not a crate
    {PropType::RockSmall, "rock_09", 0.0f, 1.0f, {-0.061f, -0.037f, -0.001f, 0.084f, 0.037f, 0.032f}, true, {-0.049f, -0.030f, 0.0f, 0.067f, 0.030f, 0.026f}},
    {PropType::ShrubA, "shrub_03", 0.0f, 1.2f, {-0.073f, -0.073f, -0.002f, 0.079f, 1.293f, 0.401f}, false, {}},     // vegetation you walk through
    {PropType::ShrubB, "shrub_sorrel_01", 0.0f, 4.0f, {-0.124f, -0.186f, -0.002f, 0.023f, 0.166f, 0.057f}, false, {}},
    {PropType::Barrel, "barrel_03", 0.0f, 1.0f, {-0.319f, -0.317f, 0.0f, 0.319f, 0.317f, 0.930f}, false, {}},        // a physical body in the simulation, not static collision
    {PropType::BarrelStack, "wooden_barrels_01", 0.0f, 1.0f, {-1.933f, -2.332f, -0.040f, 1.412f, 2.108f, 0.917f}, false, {}},  // a multi-piece layout: needs per-piece hulls (backlog)
    {PropType::Bench, "painted_wooden_bench", 0.0f, 0.7f, {-0.254f, -0.576f, -0.001f, 0.243f, 0.589f, 0.889f}, true, {-0.254f, -0.576f, -0.001f, 0.243f, 0.589f, 0.889f}},
    // Procedural trees: the trunk is the collision (the canopy is above head height for a walker, and a car's roof is below it).
    {PropType::TreePine, nullptr, 0.0f, 1.0f, {-2.2f, -2.2f, 0.0f, 2.2f, 2.2f, 9.5f}, true, {-0.30f, -0.30f, 0.0f, 0.30f, 0.30f, 6.0f}},
    {PropType::TreeRound, nullptr, 0.0f, 1.0f, {-2.0f, -2.0f, 0.0f, 2.0f, 2.0f, 6.0f}, true, {-0.28f, -0.28f, 0.0f, 0.28f, 0.28f, 4.0f}},
};

PlacedBox place(const PropInstance& p, const PropBox& b, const PropSpec& s, bool scaled_by_instance)
{
    const double k = static_cast<double>(s.scale) * (scaled_by_instance ? static_cast<double>(p.scale) : 1.0);
    // the box in the local frame after yaw_offset: rotate the corners' (x, y) by yaw_offset, take the bounds
    const double c = std::cos(static_cast<double>(s.yaw_offset)), sn = std::sin(static_cast<double>(s.yaw_offset));
    double lo_x = 1e30, lo_y = 1e30, hi_x = -1e30, hi_y = -1e30;
    for (const double x : {static_cast<double>(b.min_x), static_cast<double>(b.max_x)}) {
        for (const double y : {static_cast<double>(b.min_y), static_cast<double>(b.max_y)}) {
            const double rx = c * x - sn * y, ry = sn * x + c * y;
            lo_x = std::min(lo_x, rx); hi_x = std::max(hi_x, rx); lo_y = std::min(lo_y, ry); hi_y = std::max(hi_y, ry);
        }
    }
    PlacedBox out;
    const double mx = (lo_x + hi_x) * 0.5 * k, my = (lo_y + hi_y) * 0.5 * k;
    const double h = static_cast<double>(p.yaw);
    out.cx = p.x + std::cos(h) * mx - std::sin(h) * my;
    out.cy = p.y + std::sin(h) * mx + std::cos(h) * my;
    out.half_length = (hi_x - lo_x) * 0.5 * k;
    out.half_width = (hi_y - lo_y) * 0.5 * k;
    out.z_min = p.z + static_cast<double>(b.min_z) * k;
    out.z_max = p.z + static_cast<double>(b.max_z) * k;
    out.heading = h;
    return out;
}
} // namespace

const PropSpec& prop_spec(PropType type)
{
    for (const PropSpec& s : table) {
        if (s.type == type) return s;
    }
    throw std::invalid_argument("no prop spec for this type");
}

PlacedBox placed_visual_box(const PropInstance& p)
{
    const PropSpec& s = prop_spec(p.type);
    return place(p, s.visual, s, true);
}

PlacedBox placed_hull_box(const PropInstance& p)
{
    const PropSpec& s = prop_spec(p.type);
    return place(p, s.hull, s, s.hull_follows_scale);
}

void append_box_hull(std::vector<Triangle>& out, const PlacedBox& b, double ox, double oy, double sink)
{
    const double c = std::cos(b.heading), sn = std::sin(b.heading);
    const auto corner = [&](double lx, double ly, double z) {
        return Vec3{static_cast<float>(b.cx + c * lx - sn * ly - ox), static_cast<float>(b.cy + sn * lx + c * ly - oy), static_cast<float>(z)};
    };
    const double hl = b.half_length, hw = b.half_width, z0 = b.z_min - sink, z1 = b.z_max;
    const Vec3 v[8] = {corner(-hl, -hw, z0), corner(hl, -hw, z0), corner(hl, hw, z0), corner(-hl, hw, z0), corner(-hl, -hw, z1), corner(hl, -hw, z1), corner(hl, hw, z1), corner(-hl, hw, z1)};
    static const int faces[12][3] = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4}, {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
    for (const auto& f : faces) out.push_back({v[f[0]], v[f[1]], v[f[2]]});
}

} // namespace saige::horizon
