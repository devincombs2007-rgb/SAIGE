#include "saige/render_reference/light_culling.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::render_reference {

bool ClusterGrid::valid() const noexcept
{
    return nx >= 1 && ny >= 1 && nz >= 1 && nx <= 256 && ny <= 256 && nz <= 256 && std::isfinite(near_plane) && std::isfinite(far_plane) && near_plane > 0.0 && far_plane > near_plane && std::isfinite(tan_half_fov_y) &&
           tan_half_fov_y > 0.0 && std::isfinite(aspect) && aspect > 0.0;
}

double slice_depth(const ClusterGrid& grid, int k)
{
    return grid.near_plane * std::pow(grid.far_plane / grid.near_plane, static_cast<double>(k) / static_cast<double>(grid.nz));
}

Aabb cluster_aabb(const ClusterGrid& grid, int ix, int iy, int iz)
{
    const double z0 = slice_depth(grid, iz), z1 = slice_depth(grid, iz + 1);
    const double x0 = -1.0 + 2.0 * ix / grid.nx, x1 = -1.0 + 2.0 * (ix + 1) / grid.nx;
    const double y0 = -1.0 + 2.0 * iy / grid.ny, y1 = -1.0 + 2.0 * (iy + 1) / grid.ny;
    const double tx = grid.tan_half_fov_y * grid.aspect, ty = grid.tan_half_fov_y;
    Aabb box{{1e300, 1e300, 1e300}, {-1e300, -1e300, -1e300}};
    for (const double z : {z0, z1})
        for (const double nx : {x0, x1})
            for (const double ny : {y0, y1}) {
                const std::array<double, 3> p{nx * tx * z, ny * ty * z, z};
                for (int a = 0; a < 3; ++a) {
                    box.lo[static_cast<size_t>(a)] = std::min(box.lo[static_cast<size_t>(a)], p[static_cast<size_t>(a)]);
                    box.hi[static_cast<size_t>(a)] = std::max(box.hi[static_cast<size_t>(a)], p[static_cast<size_t>(a)]);
                }
            }
    return box;
}

std::vector<Aabb> build_cluster_boxes(const ClusterGrid& grid)
{
    if (!grid.valid()) throw std::invalid_argument("build_cluster_boxes: invalid grid");
    std::vector<Aabb> out;
    out.reserve(static_cast<size_t>(grid.count()));
    for (int iz = 0; iz < grid.nz; ++iz)
        for (int iy = 0; iy < grid.ny; ++iy)
            for (int ix = 0; ix < grid.nx; ++ix) out.push_back(cluster_aabb(grid, ix, iy, iz));
    return out;
}

bool sphere_meets_aabb(const std::array<double, 3>& c, double r, const Aabb& b)
{
    double d2 = 0.0;
    for (size_t a = 0; a < 3; ++a) {
        const double v = std::max({b.lo[a] - c[a], 0.0, c[a] - b.hi[a]});
        d2 += v * v;
    }
    return d2 <= r * r;
}

CullResult cull_lights(const ClusterGrid& grid, const std::vector<Aabb>& boxes, const std::vector<CullLight>& lights, uint32_t max_per_cluster)
{
    if (!grid.valid() || boxes.size() != static_cast<size_t>(grid.count()) || max_per_cluster < 1) throw std::invalid_argument("cull_lights: invalid arguments");
    CullResult out;
    const size_t n = boxes.size();
    out.counts.assign(n, 0u);
    out.overflow.assign(n, 0u);
    out.indices.assign(n * max_per_cluster, 0xFFFFFFFFu);
    for (size_t c = 0; c < n; ++c) {
        for (size_t i = 0; i < lights.size(); ++i) {
            if (!sphere_meets_aabb(lights[i].view_position, lights[i].radius, boxes[c])) continue;
            if (out.counts[c] < max_per_cluster) out.indices[c * max_per_cluster + out.counts[c]++] = static_cast<uint32_t>(i);
            else ++out.overflow[c];
        }
    }
    return out;
}

} // namespace saige::render_reference
