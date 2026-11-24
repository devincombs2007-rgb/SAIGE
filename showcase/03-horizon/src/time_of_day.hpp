// Showcase 03: the sun over the film and what depends only on it. Header-only so the checks in horizon_test can evaluate the same functions the render app does.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace saige::horizon {

struct SunKey {
    double t, elevation_deg, azimuth_deg;
};
// Sunrise ahead of the town, noon over the drive, a low sun behind the walker, dusk and night at the overlook.
inline constexpr SunKey sun_keys[] = {{0, 4.0, 12}, {6, 5.0, 12}, {14, 7.5, 10}, {24, 11.5, 2}, {47, 24, -15}, {75, 44, -45}, {91, 43, -75}, {100, 38, -95}, {108, 22, -125}, {116, 9, -150},
                                      {121, 3.5, -158}, {126, -0.5, -163}, {131, -6.5, -170}, {136, -12.5, -176}, {142, -17.0, -180}};

[[nodiscard]] inline double smooth01(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}

// The sun's elevation and azimuth in degrees at video time t (eased between the keys).
inline void sun_angles(double t, double& elevation_deg, double& azimuth_deg)
{
    constexpr std::size_t n = sizeof(sun_keys) / sizeof(sun_keys[0]);
    std::size_t i = 0;
    while (i + 2 < n && t > sun_keys[i + 1].t) ++i;
    const double u = smooth01((t - sun_keys[i].t) / (sun_keys[i + 1].t - sun_keys[i].t));
    elevation_deg = sun_keys[i].elevation_deg + (sun_keys[i + 1].elevation_deg - sun_keys[i].elevation_deg) * u;
    azimuth_deg = sun_keys[i].azimuth_deg + (sun_keys[i + 1].azimuth_deg - sun_keys[i].azimuth_deg) * u;
}

// How much of their emission windows and lamps show, as a continuous function of the sun's elevation: off by day, ramping up through dusk to full a little after the sun is well below the horizon.
// This replaced a four-level switch at 6, 1 and -4 degrees: each switch changed every lit surface in a single frame, which popped (horizon_test measures the largest per-frame step of this ramp).
[[nodiscard]] inline double emissive_gain_at(double elevation_deg)
{
    const double s = smooth01((7.0 - elevation_deg) / 13.0);
    return s * std::sqrt(s);
}

} // namespace saige::horizon
