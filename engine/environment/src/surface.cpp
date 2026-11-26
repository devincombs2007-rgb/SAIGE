#include "saige/env/surface.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::env {

namespace {

constexpr double kFillRatePerSecondAtOneMm = 1.0 / 25.0;  // a surface that gets 1 mm/h is nearly wet in a minute or two
constexpr double kDryRateBase = 1.0 / 1800.0;             // 30 minute time constant in still, shaded air
constexpr double kPuddleCapacityMm = 3.0;
constexpr double kPuddleEvaporationPerSecond = 1.0 / 10800.0;  // three hours
constexpr double kMaxSubstep = 2.0;

double smoothstep(double a, double b, double x)
{
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

bool unit_range(double v) { return std::isfinite(v) && v >= 0.0 && v <= 1.0; }

} // namespace

void step_surface(SurfaceWater& water, const SurfaceProperties& surface, const WeatherState& weather, double sun_exposure, double dt)
{
    if (!std::isfinite(dt) || dt < 0.0 || !unit_range(sun_exposure) || !unit_range(surface.porosity) || !(surface.drain_mm_per_hour >= 0.0))
        throw std::invalid_argument("step_surface: dt must be finite and >= 0, the sun exposure and porosity in [0, 1] and the drainage >= 0");
    double left = dt;
    const double rain = std::max(weather.rain_mm_per_hour, 0.0);
    const double fill = kFillRatePerSecondAtOneMm * std::min(rain, 4.0);  // saturates: past 4 mm/h the film is as wet as it gets, faster
    const double dry = kDryRateBase * (1.0 + 0.15 * weather.wind_speed) * (0.3 + 0.7 * sun_exposure * weather.sun_visibility);
    const double rate = fill + dry;
    const double equilibrium = rate > 0.0 ? fill / rate : 0.0;
    while (left > 0.0) {
        const double h = std::min(left, kMaxSubstep);
        left -= h;
        water.wetness = std::clamp(equilibrium + (water.wetness - equilibrium) * std::exp(-rate * h), 0.0, 1.0);
        if (surface.allows_puddles) {
            // Water only stands once the surface is saturated, and only the rain beyond what the surface drains.
            const double excess_mm_per_second = std::max(rain - surface.drain_mm_per_hour, 0.0) / 3600.0;
            const double inflow = excess_mm_per_second / kPuddleCapacityMm * smoothstep(0.85, 0.98, water.wetness);
            const double evaporation = kPuddleEvaporationPerSecond * (0.4 + 0.6 * sun_exposure * weather.sun_visibility) * (1.0 + 0.05 * weather.wind_speed);
            water.puddle = std::clamp(water.puddle + (inflow - evaporation * water.puddle) * h, 0.0, 1.0);
        } else {
            water.puddle = 0.0;
        }
    }
}

WetMaterial wet_surface(const std::array<double, 3>& albedo, double roughness, const SurfaceProperties& surface, const SurfaceWater& water)
{
    for (const double a : albedo)
        if (!unit_range(a)) throw std::invalid_argument("wet_surface: the albedo must be in [0, 1]");
    if (!unit_range(roughness) || !unit_range(surface.porosity) || !unit_range(water.wetness) || !unit_range(water.puddle))
        throw std::invalid_argument("wet_surface: the roughness, porosity, wetness and puddle must be in [0, 1]");

    WetMaterial out;
    const double exponent = 1.0 + 0.6 * surface.porosity * water.wetness;  // 1 when dry: pow(a, 1) is exact
    for (std::size_t i = 0; i < 3; ++i) out.albedo[i] = exponent == 1.0 ? albedo[i] : std::pow(albedo[i], exponent);
    // A film of water fills the micro-roughness; smooth, non-porous surfaces carry a film more readily than porous ones.
    const double film = water.wetness * (0.25 + 0.75 * (1.0 - surface.porosity));
    constexpr double kFilmRoughness = 0.12;
    const double r = roughness > kFilmRoughness ? roughness + (kFilmRoughness - roughness) * film : roughness;  // a film only ever smooths
    out.puddle_mask = smoothstep(0.05, 0.35, water.puddle);
    constexpr double kPuddleRoughness = 0.02;
    out.roughness = r + (kPuddleRoughness - r) * out.puddle_mask;
    if (water.wetness == 0.0 && water.puddle == 0.0) out.roughness = roughness;
    out.normal_flatten = out.puddle_mask;
    return out;
}

} // namespace saige::env
