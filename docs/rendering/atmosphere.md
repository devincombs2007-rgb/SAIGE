# Atmosphere and time of day (first slice)

A single-scattering sky for the scene renderer, and the sun's place in the sky as pure CPU code. Opt-in: `RendererSettings::atmosphere.enabled` is **off by default**, and then nothing
changes (the flat `sky` clear color, no sky pass, no extra resource).

## What it does

* **`sky_radiance(settings, view_dir, to_sun)`** (`saige/render/atmosphere.hpp`, CPU, double): the classic ray-marched single-scattering integral (Nishita et al.). A view ray from the
  observer crosses a spherical shell of air whose Rayleigh and Mie densities fall off exponentially with height; 16 samples along the ray and 8 from each toward the sun. Rayleigh
  and Mie phase functions, Mie extinction 1.1 times its scattering, the planet's shadow, and a ray that meets the ground integrates the air in front of it. Kilometers and per-kilometer
  coefficients throughout (the Earth's by default); linear scene-referred RGB, no exposure.
* **`shaders/scene/sky.hlsl`**: the GPU twin, step for step, in float. A full-screen triangle behind everything (no depth test or write). In an LDR scene the radiance times the exposure goes
  through `1 - exp(-x)`; in HDR it is left to the tone map.
* **Fog follows the sky**: with the atmosphere on, the fog color (and so the imported models' ambient sky color, which is 0.6 times it) is the mean sky radiance around the horizon
  (`sky_horizon_color`, eight azimuths at 3 degrees), display-referred in an LDR scene. Distant things fade into the sky they are seen against.
* **`sun_transmittance(settings, to_sun)`**: the RGB fraction of sunlight that reaches the observer (0 below the horizon): multiply a base sun color by it so the sun reddens as it sets.
  A host feeds it to `SceneLighting::sun_color`.
* **`solar_position(day_of_year, local_solar_hour, latitude, north_yaw)`**: elevation, azimuth (from north toward east) and the world-space `to_sun` vector, from Cooper's declination and the hour
  angle. `local_solar_hour` is solar time (12 is noon), so no time zone or equation of time is involved. World +X is north (rotate with `north_yaw`), so east is -Y.

The sun direction used by the sky pass is `SceneLighting::sun_direction`, normalized, the same vector the model lighting and the shadows use: they cannot disagree.

## Limits (be careful what you conclude from the images)

* **Fixed at construction.** The atmosphere and the sun direction do not change while a renderer lives, because the model renderer's scene constants (sun, ambient, fog) are immutable. A running time of
  day needs those constants to become per-frame (a ring like the shadow block) and the shadows' sun to follow; that is the next slice. `solar_position`, `sun_transmittance` and `sky_horizon_color` are
  ready for it.
* Single scattering only (no multiple scattering: the sky is too dark near the sun at low elevation and the horizon slightly too saturated), no ozone (the zenith at sunset is too red-orange rather than
  blue-purple), no ground albedo, no sun disc, no clouds, and no aerial perspective on geometry beyond the exponential fog.
* The 0.1 cube and ground shaders keep their own fixed lighting.
* Software Vulkan (lavapipe) has validated correctness only; no performance claim. The sky pass costs 16 x 8 exponentials per pixel, to be measured on hardware (a lookup table is the usual remedy if it matters).

## Verified

`tests/test_atmosphere.cpp` (CPU): validation; a blue zenith whose blue-to-red ratio is Rayleigh's (3.5 to 5.7 without aerosol, whitened with it); the Mie halo around the sun; a brighter, whiter horizon; sunlight
reddening monotonically as the sun lowers (red passes best, blue is nearly gone at the horizon, zero below it); finite, non-negative radiance in every direction and for every sun elevation including night;
the planet's shadow (a night sky 1000 times darker); linearity in the sun intensity; rotational invariance about the vertical; the horizon color following the sun and the exposure; solar position at the
equinox and solstices (known elevations and azimuths, sunrise and sunset east and west, midnight, the polar night and midnight sun, symmetry about noon, the yaw, invalid input).
`tests/test_sky.cpp` (Vulkan, validation required): 400 probes over four sun elevations (60, 20, 3 and -3 degrees) and four camera pitches agree with the CPU reference to 0.012 in a channel (8-bit
quantization and float against double); the sun's side is redder and brighter at sunset; exposure 0 is black; fogged distant ground equals the horizon color to 0.002; without the atmosphere the clear color
is unchanged. `test_scene_renderer.cpp` (null device): off creates nothing for the sky, the sky's two shaders and its pipeline are created on the first frame, each of their creations failing in turn is a
failed frame that leaks nothing and leaves a working renderer, and invalid settings are refused before anything is acquired.
