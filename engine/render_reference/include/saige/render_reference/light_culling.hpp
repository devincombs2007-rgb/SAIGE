// Clustered light culling, CPU reference (docs/rendering/local-lights.md, step 3). The view frustum is cut into nx x ny screen tiles and nz exponential depth slices; a light (a sphere of its radius, in view space) is
// listed in every cluster whose axis-aligned box the sphere meets. The GPU pass must produce a SUPERSET of this exact list (a missing light is a visible hole in the lighting; an extra one only costs time) and, here, the
// same list within float rounding. The list of a cluster is in ascending light index, truncated at `max_per_cluster` with the overflow counted: the order and the truncation are part of the contract (deterministic output).
//
// View space: x right, y up, z forward (positive depth), the camera at the origin. Slice k spans depth near * (far / near)^(k / nz) to near * (far / near)^((k + 1) / nz). A cluster's box is the box of the eight corners of its
// frustum cell (the corners at the slice's near and far depth, at the tile's NDC rectangle), so it contains the cell (and a little more).
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace saige::render_reference {

struct ClusterGrid {
    int nx = 16, ny = 9, nz = 24;
    double near_plane = 0.1, far_plane = 200.0;
    double tan_half_fov_y = 0.5773502691896257;  ///< 60 degrees vertical
    double aspect = 16.0 / 9.0;
    [[nodiscard]] int count() const noexcept { return nx * ny * nz; }
    [[nodiscard]] bool valid() const noexcept;
};

struct Aabb {
    std::array<double, 3> lo, hi;
};

/// Slice boundaries: depth at the near face of slice k (k = nz gives the far plane).
[[nodiscard]] double slice_depth(const ClusterGrid& grid, int k);
/// The box of cluster (ix, iy, iz), index = (iz * ny + iy) * nx + ix.
[[nodiscard]] Aabb cluster_aabb(const ClusterGrid& grid, int ix, int iy, int iz);
[[nodiscard]] std::vector<Aabb> build_cluster_boxes(const ClusterGrid& grid);

/// A sphere (centre in view space, radius) meets a box: the squared distance from the centre to the box is at most radius^2.
[[nodiscard]] bool sphere_meets_aabb(const std::array<double, 3>& centre, double radius, const Aabb& box);

struct CullResult {
    std::vector<uint32_t> counts;    ///< per cluster: lights listed (at most max_per_cluster)
    std::vector<uint32_t> indices;   ///< per cluster, max_per_cluster slots: light indices ascending; unused slots hold 0xFFFFFFFF
    std::vector<uint32_t> overflow;  ///< per cluster: lights that met the cluster but did not fit
};

struct CullLight {
    std::array<double, 3> view_position;
    double radius;
};

/// The reference culling: every cluster against every light, ascending light index, truncated.
[[nodiscard]] CullResult cull_lights(const ClusterGrid& grid, const std::vector<Aabb>& boxes, const std::vector<CullLight>& lights, uint32_t max_per_cluster);

} // namespace saige::render_reference
