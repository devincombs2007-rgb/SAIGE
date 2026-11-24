// SAIGE gfx_assets — an image-based-lighting environment derived from the atmosphere, so the reflections and the ambient of the models agree with the sky they stand under.
//
// `sky_environment_image` evaluates, for every texel of an equirectangular image (the engine's mapping, see environment.hpp), the radiance the sky pass would show toward it:
// `render::sky_radiance` times the atmosphere's exposure above the horizon, and below it a Lambertian ground that reflects the sun and the sky:
//
//   ground = albedo * (sun_irradiance * max(0, sun.z) * T(sun) + cosine-weighted mean of the sky above the horizon)
//
// (`sun_irradiance` is in the units of `SceneLighting::sun_color`: 1 is a sun whose diffuse reflection off a white horizontal surface is 1; `T` is the air's transmittance toward the sun, so
// the ground goes dark and red at dusk and black at night). The sun's disc, the moon and the stars are *not* in the image: the sun is the directional light's job (its specular highlight
// and its shadow), and a bright disc is exactly what SH9 represents badly. `sky_environment` then runs the standard precompute (SH9 diffuse and the GGX-prefiltered roughness chain).
//
// Cost is CPU-only and a function of the settings: the default 128 x 64 source costs about 0.1 s in a debug build and is meant to be run when the time of day has moved enough to matter (a few
// times per game hour), not every frame. The renderer takes an environment at construction today (`RendererSettings::environment`); replacing it while a renderer lives is future work, so a
// host that wants a changing environment rebuilds the renderer for now (see docs/rendering/sky-environment.md).
#pragma once

#include "saige/gfx_assets/environment.hpp"
#include "saige/render/atmosphere.hpp"

#include <string>

namespace saige::gfx {

struct SkyEnvironmentSettings {
    uint32_t source_width = 128;                        ///< the radiance image's width (its height is half): even, 16 to 1024
    float ground_albedo = 0.2f;                         ///< 0 to 1: how much of the light on the ground it reflects (0: a black ground)
    render::Float3 sun_irradiance{1.0f, 0.95f, 0.85f};  ///< the sun's color as `SceneLighting::sun_color` has it; scaled by the air's transmittance toward the sun
    /// Moonlight and airglow: a small radiance added to the sky above the horizon as the sun sinks (nothing by day, all of it from about 8 degrees below the horizon down), so the models and the
    /// ground are not pitch black at night. The single-scattering sky is black once the sun is a few degrees down, because it has no multiple scattering, ozone or moon; this is the stand-in.
    /// {0, 0, 0} turns it off.
    render::Float3 night_ambient{0.015f, 0.021f, 0.040f};
    EnvironmentSettings filter{128, 5, 64, 0.0f};       ///< the precompute: level 0 width, roughness levels, samples per texel, diffuse limit
};

/// Throws std::invalid_argument for a value outside the ranges above (or invalid atmosphere settings, or a zero or non-finite sun direction).
[[nodiscard]] HdrImage sky_environment_image(const render::AtmosphereSettings& atmosphere, render::Float3 to_sun, const SkyEnvironmentSettings& settings = {});

/// The image, precomputed. `error` is set (and `environment` empty) if the precompute rejects it; invalid arguments throw as above.
[[nodiscard]] EnvironmentResult sky_environment(const render::AtmosphereSettings& atmosphere, render::Float3 to_sun, const SkyEnvironmentSettings& settings = {});

} // namespace saige::gfx
