// Wet surfaces: how much water a surface holds under a given weather, and what that does to its material.
//
// Wetness w in [0, 1] (a damp film) follows w' = fill (1 - w) - dry w, with fill from the rain rate and dry from wind, sun and the surface; the equation is integrated exactly (w moves toward its
// equilibrium fill / (fill + dry) by exp(-(fill + dry) dt)), so it stays in [0, 1] for any step size. Puddles are a second store: standing water p in [0, 1] that only collects once the surface is
// saturated and the rain outruns the surface's drainage, and that evaporates slowly, so a puddle outlives the damp film (which is what makes a street shine after the rain has stopped).
// `wet_surface` is the material model the renderer applies: wet porous surfaces darken (albedo raised to a power, so black and white are fixed points), a water film smooths the micro-roughness, and a
// puddle is a mirror-smooth, normal-flattened patch. With no water the output is bit-identical to the input.
#pragma once

#include "saige/env/weather.hpp"

#include <array>

namespace saige::env {

struct SurfaceProperties {
    double porosity = 0.3;          ///< 0..1: how strongly water darkens the surface (asphalt 0.6, concrete 0.5, polished stone 0.1, metal 0)
    double drain_mm_per_hour = 2.0; ///< rain below this rate soaks away or runs off instead of pooling
    bool allows_puddles = true;     ///< false on slopes, roofs and walls
};

struct SurfaceWater {
    double wetness = 0.0;  ///< 0..1
    double puddle = 0.0;   ///< 0..1 (1 is a full 3 mm of standing water)
};

/// Advance one surface by dt seconds (finite, >= 0) under `weather`; `sun_exposure` in [0, 1] is how much of the direct sun the surface receives (shade dries slowly).
void step_surface(SurfaceWater& water, const SurfaceProperties& surface, const WeatherState& weather, double sun_exposure, double dt);

struct WetMaterial {
    std::array<double, 3> albedo{};
    double roughness = 0.0;
    double puddle_mask = 0.0;     ///< 0..1: blend toward the mirror-smooth puddle in the shader
    double normal_flatten = 0.0;  ///< 0..1: how far to flatten the normal map (standing water is level)
};

/// The wet look of a material (`albedo` linear 0..1 per channel, `roughness` 0..1). Throws std::invalid_argument for non-finite or out-of-range inputs.
[[nodiscard]] WetMaterial wet_surface(const std::array<double, 3>& albedo, double roughness, const SurfaceProperties& surface, const SurfaceWater& water);

} // namespace saige::env
