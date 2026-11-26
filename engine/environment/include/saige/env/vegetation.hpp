// Vegetation: wind-driven bending, leaf flutter, and a stable, density-driven grass scatter with distance thinning that never pops.
//
// These are CPU references for what a vertex shader and an instancing pass will do, so the shader port has numbers to match and the behaviour is tested without a GPU.
//
// * `sway_angle` and `bend_position`: a plant leans downwind with the wind's dynamic pressure (angle grows with speed squared and saturates), oscillates at a natural frequency that falls with the plant's height,
//   and bends by rotating each vertex about the plant's base by an angle that grows with its height fraction. A rotation about the base keeps every vertex's distance from the base, so branches do not stretch.
// * `leaf_flutter`: a small, bounded, high-frequency offset along a vertex's normal, scaled by a painted mask and by the wind.
// * `scatter_grass`: a jittered grid with hashed positions (so the same world position always grows the same blade, whatever region is asked for), a density mask, and `lod_keep`, which thins instances with distance by comparing a
//   per-instance hash with a falling probability: the instances kept at a far distance are a subset of those kept nearer, so thinning never makes a blade appear as the camera moves away, and `lod_width_scale`
//   widens the survivors to hold the coverage.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace saige::env {

struct PlantWind {
    double height_m = 6.0;      ///< the plant's height; the natural frequency falls with it
    double stiffness = 1.0;     ///< > 0; 2 leans half as far as 1 for the same wind
    double max_angle = 0.35;    ///< radians, the lean the wind approaches but does not exceed (about 20 degrees)
};

/// The lean angle of the whole plant (radians, >= 0) for a wind speed (m/s) at time t, with a per-plant phase so a field does not sway in unison. Throws std::invalid_argument for invalid parameters.
[[nodiscard]] double sway_angle(const PlantWind& plant, double wind_speed, double t, double phase);
/// The natural frequency in Hz of a plant of this height (0.5 / sqrt(height), so a 6 m tree sways at about 0.2 Hz and a 0.5 m shrub at 0.7 Hz).
[[nodiscard]] double natural_frequency_hz(double height_m);

/// Bend a vertex. `local` is the vertex position relative to the plant's base (z up), `wind_dir` the horizontal direction the wind blows toward (need not be normalised), `angle` the lean from `sway_angle`.
/// The vertex rotates about the base around the horizontal axis perpendicular to the wind by angle * (z / height)^1.5 (clamped to the plant's height), so the tip leans furthest and the base stays put.
[[nodiscard]] std::array<double, 3> bend_position(const std::array<double, 3>& local, double height_m, const std::array<double, 2>& wind_dir, double angle);

/// A flutter offset (metres) along `normal`: at most 0.03 m * mask at a wind of 12 m/s or more, 0 in still air or at mask 0.
[[nodiscard]] std::array<double, 3> leaf_flutter(const std::array<double, 3>& normal, double mask, double wind_speed, double t, double phase);

struct GrassConfig {
    std::uint64_t seed = 1;
    double cell = 0.35;     ///< m, one blade cluster per cell at full density
    double jitter = 0.8;    ///< 0..1, how much of the cell a cluster may wander; the minimum spacing is cell * (1 - jitter) along an axis
    double near = 25.0;     ///< m, full density up to here
    double far = 90.0;      ///< m, the thinning reaches `min_fraction` here
    double min_fraction = 0.08;
};

struct GrassInstance {
    double x = 0.0, y = 0.0;
    double yaw = 0.0;    ///< radians
    double scale = 1.0;  ///< 0.7..1.3
    std::uint64_t id = 0;  ///< stable: a function of the cell and the seed only
};

/// The clusters inside [x0, x1) x [y0, y1) (with x1 > x0 and y1 > y0), in cell order. `density` (optional) maps a world position to [0, 1]; a cluster grows where its own hash is below the density there.
[[nodiscard]] std::vector<GrassInstance> scatter_grass(const GrassConfig& config, double x0, double y0, double x1, double y1, const std::function<double(double, double)>& density = {});
/// The fraction of instances kept at `distance` metres: 1 up to `near`, then falling smoothly to `min_fraction` at `far` and staying there.
[[nodiscard]] double lod_fraction(const GrassConfig& config, double distance);
/// Whether an instance is kept at that distance. Monotonic in distance: kept far implies kept near.
[[nodiscard]] bool lod_keep(const GrassConfig& config, const GrassInstance& instance, double distance);
/// 1 / sqrt(lod_fraction): the width factor that holds the covered area when instances are thinned.
[[nodiscard]] double lod_width_scale(const GrassConfig& config, double distance);

} // namespace saige::env
