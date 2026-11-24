# Sky-derived environment lighting and the recommended outdoor settings

`gfx::sky_environment(atmosphere, to_sun, settings)` (gfx_assets/sky_environment.hpp) builds the image-based-lighting environment (#82/#85) from the atmosphere itself, so the models' reflections and ambient
agree with the sky they stand under, at every time of day, with no external HDR file.

## What it computes

For every texel of an equirectangular image (the engine's mapping), the radiance the sky pass shows toward it: `render::sky_radiance` times the atmosphere's exposure above the horizon, and below the
horizon a Lambertian ground:

```
ground = albedo * (sun_irradiance * max(0, sun.z) * T(sun) + cosine-weighted mean of the sky above the horizon)
```

`sun_irradiance` is in the units of `SceneLighting::sun_color` (1 = a sun whose diffuse reflection off a white horizontal surface is 1); `T` is the air's transmittance toward the sun, so the ground goes
warm and dark at dusk. A faint bluish **night ambient** (moonlight and airglow, default `{0.015, 0.021, 0.040}` scene radiance) fades in as the sun sinks from 1 degree to about 8 degrees below the horizon,
because the single-scattering sky is black once the sun is a few degrees down. The sun disc, moon and stars are not in the image (the sun is the directional light's job; a bright disc is what SH9 represents
badly); the standard precompute (SH9 diffuse, GGX-prefiltered roughness chain) follows. The image is deterministic. Default cost in a debug build: about 45 ms for the 128 x 64 image and 70 ms for the
precompute: meant to run when the time of day has moved enough to matter (a few times per game hour), never per frame.

**Limit:** the renderer takes its environment at construction (`RendererSettings::environment`). Replacing it while a renderer lives is not implemented; a host that wants a changing environment rebuilds the
renderer (or keeps the sun fixed for the IBL and moves only the directional light, which is cheaper and often enough for a short sequence). A per-frame-slot environment swap is the next step.

## Recommended outdoor settings (what `test_living_sky.cpp` runs)

| Setting | Value | Why |
| --- | --- | --- |
| `hdr` | `true` | the sky and the lit scene exceed 1 |
| `tone_map` | ACES fit, `exposure_ev = 0` | the sky and the lit ground land mid-range at day with no exposure tweak |
| `model_shading` | `MetallicRoughness` | the only mode that reads the IBL |
| `atmosphere` | enabled, `exposure = 1`, `sun_intensity = 20` | the defaults; the sky pass and the environment use the same product |
| `environment` | `to_environment_lighting(sky_environment(atm, sun).environment, 1.0f)` | reflections match the sky |
| `dynamic_lighting` + `SceneFrame::lighting` | `time_of_day_lighting(atm, to_sun, SceneLighting{})` | sun color follows the air, ambient fades to the night floor |
| `shadows` | 3 cascades, 1024 tiles, `distance = 60` | sharp near, reaches far; the sun sets before the cascades degrade |
| `fog_density` | `0.006` | distant ground fades into the horizon colour |
| `ground_half_size` | `0.01` (hide the built-in checkerboard) and draw a model ground | the 0.1 checkerboard has fixed lighting and stays bright at night |

Measured on lavapipe (software Vulkan; display values 0..1 of the top and bottom fifths of a 320 x 180 frame, a scene of four PBR cubes on a lit ground, sun azimuth 35 degrees in front of the camera):

| Sun elevation | Sky (mean rgb) | Ground (mean rgb) | Pure white | Black |
| --- | --- | --- | --- | --- |
| 62 deg (noon) | 0.34 0.53 0.69 | 0.59 0.58 0.54 | 0.00% | 0.00% |
| 28 deg | 0.52 0.62 0.72 | 0.38 0.39 0.36 | 0.00% | 0.00% |
| 4 deg (sunset) | 0.27 0.33 0.29 | 0.13 0.11 0.05 (warm) | 0.00% | 0.17% |
| -7 deg (dusk) | 0.00 | 0.002 0.003 0.007 | 0.00% | 55% |
| -35 deg (night) | 0.00 | 0.002 0.003 0.007 | 0.00% | 58% |

The test asserts these properties (not the exact numbers): day sky luminance between 0.08 and 0.85 with under 2% pure white and under 10% black; the noon sky is bluer than red and the sunset relatively redder; brightness
falls monotonically from noon to night; the night is dark, not blown, and the lit ground goes dark with the sun. With `SAIGE_CAPTURE_DIR` set it also writes each frame as a binary PPM to look at.

**What the frames show.** Noon and afternoon: a blue gradient sky, a bright haze around the sun, long soft cascade shadows, warm-grey ground, reflective cubes picking up the sky. Sunset: a warm band at the horizon,
teal overhead, warm ground. Dusk and night: nearly black in the sky (see below), the models faintly moonlit blue.

## Limits of the single-scattering sky for night and twilight

The sky is black from a few degrees below the horizon until the stars and moon (#96, opt-in) are on: there is no twilight glow (no multiple scattering or ozone), so a showcase should either stop the sun at about
-3 degrees, turn on the stars and moon, or accept a dark sky with moonlit models. A twilight term is future work. The built-in checkerboard ground is a 0.1 placeholder with fixed lighting.

## Assets

No external asset is needed for the sky or the lighting above. If a licensed environment or props are wanted, candidates (not downloaded or verified here; each needs the licence review the project asks for before it
enters a repository): Poly Haven HDRIs (CC0, 1k-2k is plenty for the precompute) as a static IBL alternative at a fixed time of day; Kenney and Quaternius low-poly glTF packs (CC0) for props; the Khronos glTF
sample models, which carry per-model licences (some CC0 or CC-BY) and must be checked one by one.

## Verification

`engine/gfx_assets/tests/test_sky_environment.cpp`: the sky texels are the reference sky times the exposure at each texel's own direction, the ground is the formula with an independently computed cosine-weighted mean
(four suns, every texel below the horizon equal), the albedo scales it, the sun vector is normalized, the night floor, diffuse light falling with the sun from noon to night and the up/down difference by day, warmth at
sunset, the mirror level at the zenith near the sky's radiance, and the argument validation. Fourteen mutants of `sky_environment.cpp` each fail a test. `engine/rhi_vulkan/tests/test_living_sky.cpp`: the table above.
Software Vulkan evidence only; no physical-GPU timing is claimed.
