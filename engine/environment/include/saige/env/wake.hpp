// Boat wakes: the geometry of a ship's wave pattern and a foam trail a host can feed to a water shader.
//
// Facts used (deep water): a steadily moving disturbance leaves a V of waves whose half-angle is asin(1/3) = 19.47 degrees whatever the speed (Kelvin); transverse waves behind it have the wavelength
// 2 pi v^2 / g; a displacement hull's speed is limited near a Froude number v / sqrt(g L) of 0.40 (the hull speed, about 1.25 sqrt(L) m/s). The foam is not simulated waves: it is a trail of foam patches left
// at the hull's position every `spawn_interval` metres, each born with a strength that follows the speed as a fraction of hull speed, spreading slowly, and fading exponentially. `foam(x, y, t)` is the
// density of that trail at a point and time in [0, 1], for a water material to mix in white. No claim is made about a wave simulation: the wake's waves belong to the water module's wave surface, and this module
// only supplies the foam that follows the hull.
#pragma once

#include <cstddef>
#include <vector>

namespace saige::env {

inline constexpr double kKelvinHalfAngleRadians = 0.33983690945412193;  ///< asin(1/3)
inline constexpr double kGravity = 9.81;

[[nodiscard]] double froude_number(double speed, double length_m);
[[nodiscard]] double hull_speed(double length_m);                       ///< m/s at a Froude number of 0.40
[[nodiscard]] double transverse_wavelength(double speed);               ///< m, 2 pi v^2 / g
/// The lateral half-width of the wave V at `distance_behind` metres behind the vessel.
[[nodiscard]] double kelvin_half_width(double distance_behind);

struct WakeConfig {
    double hull_length = 6.0;          ///< m
    double hull_beam = 2.0;            ///< m
    double foam_seconds = 18.0;        ///< e-folding time of a patch
    double spawn_interval = 0.5;       ///< m of travel between patches
    double min_speed = 0.5;            ///< m/s, below this no foam is shed
    double spread_speed = 0.12;        ///< m/s, how fast a patch widens
    std::size_t max_patches = 4096;
};

class WakeTrail {
public:
    explicit WakeTrail(const WakeConfig& config = {});
    /// Report the hull's position (metres) and speed (m/s) at time t (seconds, never decreasing). Shed patches as the hull travels; old ones are dropped once they are invisible.
    void update(double t, double x, double y, double speed);
    /// The foam density in [0, 1] at a point and time t (>= the last update's time not required).
    [[nodiscard]] double foam(double x, double y, double t) const;
    [[nodiscard]] std::size_t patch_count() const noexcept { return patches_.size(); }

private:
    struct Patch {
        double x, y, t, strength;
    };
    WakeConfig cfg_;
    std::vector<Patch> patches_;
    double last_x_ = 0.0, last_y_ = 0.0, last_t_ = 0.0, travelled_ = 0.0;
    bool started_ = false;
};

} // namespace saige::env
