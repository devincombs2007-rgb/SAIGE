// SAIGE Render — a single-scattering atmosphere (Rayleigh and Mie) and the sun's place in the sky, as pure CPU code with a GPU twin.
//
// The sky model is the classic ray-marched single-scattering integral (Nishita et al.): a view ray from the observer through a spherical shell of air whose
// density falls off exponentially with height, 16 samples along the ray and 8 toward the sun from each. `sky_radiance` is the reference the sky shader
// (shaders/scene/sky.hlsl) must agree with, step for step (the shader is float, in kilometers; this is double). Linear scene-referred RGB radiance: no
// exposure or tone map is applied here. Units: kilometers and per-kilometer coefficients throughout.
//
// What it is not: no multiple scattering (the sky is a little too dark near the sun at low elevation and the horizon a little too saturated), no ozone (the
// zenith at sunset is bluer in reality), no ground albedo (a ray that hits the planet integrates only the air in front of it), no clouds and no sun disc yet.
// It is a physically motivated, deterministic, tested foundation for time of day, not a calibrated photometric sky.
//
// World conventions (docs/standards/coordinate-system.md): +Z up, +X forward, +Y left. The sun is described by `to_sun`, the unit vector from the scene toward
// the sun, the same direction `SceneLighting::sun_direction` normalizes to. `solar_position` turns a date, a local solar time and a latitude into that
// vector with +X as north (rotate the world with `north_yaw_radians`).
#pragma once

#include "saige/render/math.hpp"

#include <cstdint>

namespace saige::render {

inline constexpr uint32_t kAtmosphereViewSteps = 16;
inline constexpr uint32_t kAtmosphereLightSteps = 8;

struct AtmosphereSettings {
    bool enabled = false;  ///< RendererSettings: draw the sky pass and derive the fog color from the horizon
    float planet_radius_km = 6360.0f;
    float atmosphere_height_km = 60.0f;
    float rayleigh_scale_height_km = 7.994f;
    float mie_scale_height_km = 1.2f;
    Float3 rayleigh_coefficient{5.8e-3f, 13.5e-3f, 33.1e-3f};  ///< per km at sea level (the Earth's, for red, green and blue)
    float mie_coefficient = 21.0e-3f;                          ///< per km at sea level
    float mie_g = 0.76f;                                       ///< forward-scattering anisotropy, -0.99 to 0.99
    float sun_intensity = 20.0f;                               ///< radiance scale of the incoming sunlight
    float observer_height_km = 0.001f;                         ///< above the ground (1 m): the camera's altitude, which a host may update
    float exposure = 1.0f;                                     ///< scene-referred multiplier applied to the sky pass and the fog color
    // Celestial bodies, each off at 0 (the default: the sky is exactly what it was). They are added to the sky's radiance before the exposure.
    float sun_disc_intensity = 0.0f;  ///< radiance at the center of the sun's disc (0 to 1e6), times the air's transmittance toward it, so it reddens and dims as it sets; a few thousand is bright next to the sky's ~1
    float moon_intensity = 0.0f;      ///< radiance of the moon's disc, which sits opposite the sun (a full moon); times its own transmittance; hundreds of times dimmer than the sun's disc
    float star_intensity = 0.0f;      ///< peak radiance of a star, faded in as the sun sinks below the horizon and out toward the horizon (0 to 1e6)
};

/// Throws std::invalid_argument if a value is not finite, a radius, height, scale height, coefficient or intensity is not positive (the Mie
/// coefficient and the exposure may be zero), the anisotropy is outside -0.99 to 0.99, or the observer is below the ground or above the atmosphere.
void validate_atmosphere(const AtmosphereSettings& atmosphere);

/// The radiance toward `view_dir` (any non-zero direction; normalized here) with the sun at `to_sun` (normalized here), linear RGB, no exposure. A
/// direction below the horizon sees the air in front of the planet only. Pure and deterministic. Both directions must be finite and non-zero.
[[nodiscard]] Float3 sky_radiance(const AtmosphereSettings& atmosphere, Float3 view_dir, Float3 to_sun);

/// The fraction of sunlight (RGB) that reaches the observer from `to_sun` (1 outside the air, near 0 for blue and less for green at the horizon): what to
/// multiply a base sun color by so that the sun reddens as it sets. 0 when the sun is below the planet's horizon.
[[nodiscard]] Float3 sun_transmittance(const AtmosphereSettings& atmosphere, Float3 to_sun);

/// The angular radius (radians) of the sun's and the moon's discs: the real ones, about half a degree across.
inline constexpr float kSunAngularRadius = 0.00465f;
inline constexpr float kMoonAngularRadius = 0.0045f;

/// The sun's disc toward `view_dir`: `sun_disc_intensity` times the air's transmittance toward the sun, limb-darkened (1 - 0.6 (1 - mu)) with a soft edge over the outer 15% of the radius; 0 outside
/// the disc, below the horizon (the transmittance is 0 there) and when the intensity is 0. Linear RGB, no exposure. Pure; the reference of the sky shader's disc.
[[nodiscard]] Float3 sun_disc_radiance(const AtmosphereSettings& atmosphere, Float3 view_dir, Float3 to_sun);
/// The moon: the same profile, centered opposite the sun, `moon_intensity` times the transmittance toward the moon.
[[nodiscard]] Float3 moon_radiance(const AtmosphereSettings& atmosphere, Float3 view_dir, Float3 to_sun);
/// The stars toward `view_dir`: a fixed, deterministic field of point-like stars (one cell grid over the sphere, a hash of the cell deciding whether it holds a star, where, and how bright and
/// what tint), `star_intensity` times how dark the sky is (fading in as the sun's elevation falls from -1 degree to -11) times a fade toward the horizon (0 at and below it, 1 at 11.5 degrees up).
/// The field does not rotate. 0 in daylight. Linear RGB, no exposure.
[[nodiscard]] Float3 star_radiance(const AtmosphereSettings& atmosphere, Float3 view_dir, Float3 to_sun);
/// `sky_radiance` plus the three above: what the sky pass shows toward `view_dir`, before the exposure.
[[nodiscard]] Float3 sky_with_bodies(const AtmosphereSettings& atmosphere, Float3 view_dir, Float3 to_sun);

/// The mean sky radiance around the horizon (eight azimuths at 3 degrees of elevation, times exposure): the color distant things fade into, so the fog
/// follows the time of day. Linear RGB.
[[nodiscard]] Float3 sky_horizon_color(const AtmosphereSettings& atmosphere, Float3 to_sun);

/// Where the sun is: elevation above the horizon and azimuth from north toward east (radians), and the world-space unit vector toward it.
struct SolarPosition {
    double elevation = 0.0;
    double azimuth = 0.0;
    Float3 to_sun;
};

/// `day_of_year` 1 to 365 (366 allowed), `local_solar_hour` 0 to 24 (12 is solar noon, so no time zone or equation of time is involved), `latitude_degrees`
/// -90 to 90 (positive north). Declination follows Cooper's formula (23.44 degrees times the sine of the year's phase); refraction is not modeled. World +X is
/// north and +Y is west (the engine's left), so east is -Y; `north_yaw_radians` turns the world about +Z (counter-clockwise) when +X is not north. Throws
/// std::invalid_argument for a value out of range or not finite.
[[nodiscard]] SolarPosition solar_position(int day_of_year, double local_solar_hour, double latitude_degrees, double north_yaw_radians = 0.0);

} // namespace saige::render
