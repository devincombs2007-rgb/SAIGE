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

## One sun direction

The sky pass, the horizon fog and the model lighting all take their sun from `RendererSettings::lighting.sun_direction` through the same overflow-safe normalization (`unit_sun_direction`, which divides by the largest component before squaring), so every finite non-zero direction, from 1e-37 to 3e38 in size, gives the same sky, fog and material as the corresponding unit vector (`test_sky.cpp`, within one 8-bit level). A plain float `normalize` squares the components and turned such directions into a zero vector for the sky and the fog.

## Sun disc, moon and stars

Three opt-in celestial bodies on top of the single-scattering sky, each **off at intensity 0** (the default), so the sky is exactly what it was until a host sets one. They are added to the sky's radiance before the exposure, in the sky pass
(the CPU references are `sun_disc_radiance`, `moon_radiance`, `star_radiance` and their sum `sky_with_bodies`; the shader mirrors them step for step with the same constants).

```cpp
rs.atmosphere.sun_disc_intensity = 3000.0f;  // radiance at the center of the sun's disc; times the air's transmittance, so it reddens and dims as it sets
rs.atmosphere.moon_intensity = 20.0f;        // the moon's disc, opposite the sun
rs.atmosphere.star_intensity = 4.0f;         // the peak radiance of a star
```

* **Discs.** Angular radius 0.00465 rad for the sun and 0.0045 rad for the moon (about half a degree, the real ones). Seen along `d` toward a center `c`, the profile uses `sin(theta) = |d x c|` rather than `acos(d . c)`, which has no precision left at
  half a degree in float. Inside the disc the radiance is the intensity times limb darkening `1 - 0.6 (1 - mu)` with `mu = sqrt(1 - x^2)`, `x = sin(theta) / sin(radius)`, and a soft edge over the outer 15% of the radius; 0 outside. The
  intensity is multiplied by the air's transmittance toward the body (`sun_transmittance`, 64 steps), which is 0 below the planet's horizon: a body below it is hidden by the planet and a body near it is red and dim.
* **The moon is a full moon opposite the sun**: it rises as the sun sets. There is no phase, no independent orbit and no surface; a moon with its own position is future work (it needs its own direction in the per-frame lighting).
* **Stars.** A fixed, deterministic field: the view direction scaled by 64 is cut into unit cells, a hash of the cell (lowbias32, identical on the CPU and in HLSL) decides whether it holds a star (half of them), where in the cell, and its brightness and warm-to-cool
  tint. A star is a smooth falloff `(1 - r / 0.12)^2` around its point in cell units. The field is dimmed toward the horizon (full 11.5 degrees up, 0 at and below it) and fades in as the sun's elevation falls from -1 to -11 degrees; by day there are none.
  About 0.4% of the sky above 15 degrees is lit by a star. The field does not rotate with time and ignores atmospheric extinction beyond the horizon fade: it is a plausible, stable foundation, not an astronomical catalogue.
* **Aliasing.** The discs are physically sized, so at low resolutions or wide fields of view they are a few pixels or less across and can shimmer; the soft edge is a fixed fraction of the radius, not a pixel footprint. Stars are sub-pixel: a pixel
  center that misses one shows nothing. A host that wants them steadier turns on bloom (a bright disc then bleeds into its surroundings) and temporal accumulation, neither of which exists in this slice.

### Verification

`test_atmosphere.cpp` (CPU): off by default and bit-exact with the plain sky, the intensity validation, the disc's center value, its monotone falloff and symmetry, limb darkening, reddening toward the horizon, absence below it; the moon opposite the sun with its own
transmittance; stars deterministic, sparse (0.4%), bounded, absent by day, at and below the horizon, fading in with darkness. `test_sky.cpp` (validated Vulkan): the sun's disc, the moon and the stars against `sky_with_bodies` pixel by pixel (worst difference
0.005 in the display value), including a view straddling the horizon with a bright star field. Twenty mutants of the new code (every constant of the profile, the radii, the transmittance, the moon direction, the night and horizon fades, the density, the falloff, the tint, the hash, the
push-constant packing, the validation and the sum) each fail a test or do not build. Software Vulkan only.
