# Per-frame lighting (time of day)

Opt-in lighting that can change every frame: a running clock moves the sun, and the sky, the fog, the models' lighting and the shadows follow it. Off by default; with it off
nothing is allocated and every image is what it was before (checked byte for byte).

## Decisions (agreed on #43)

* **One immutable, validated payload per renderer per frame**, published into a reclaimed *frame slot*, before the frame's first draw reads it.
* Conflicting publications are rejected **before any write**. A rejected call changes nothing and does not use up the frame's publication.
* The default path is unchanged: with `dynamic_lighting` off the scene constants are one immutable buffer and one bind group, exactly as before.
* Core owns the clock and gameplay (what time it is, how fast it runs); `render` owns turning a sun direction into pixels. `time_of_day_lighting` and `solar_position` are
  pure helpers a host may use or ignore.

## Using it

```cpp
RendererSettings rs;
rs.dynamic_lighting = true;                 // off by default
rs.atmosphere.enabled = true;               // optional: a sky that follows the sun
SceneRenderer renderer(device, format, rs);

// every frame, before render():
const SolarPosition sun = solar_position(day_of_year, local_solar_hour, latitude_degrees);
const SceneLighting light = time_of_day_lighting(rs.atmosphere, sun.to_sun, day_look);
SceneFrame frame;
frame.lighting = &light;                    // nullptr: the construction-time rs.lighting
renderer.render(cmd, frame, target);
```

`SceneFrame::lighting` is not retained. `render` validates it (finite, non-negative colors, a direction with length) before it creates, records or writes anything:
`std::invalid_argument` for a bad value, `std::logic_error` if the renderer was not built with `dynamic_lighting`. Both leave the renderer exactly as it was.

### One sun for everything

`render` normalizes the frame's direction once (`unit_sun_direction`: no squaring of components, so any finite non-zero vector works) and uses that vector for

| Consumer | What follows the frame's sun |
| --- | --- |
| Sky pass | the sun direction of the Nishita integral |
| Fog and clear color | the horizon of that sky (display-referred in an LDR scene) when the atmosphere is on; `RendererSettings::sky` otherwise |
| Models | sun direction, sun color, ground ambient, and the sky color (which is both the fog color and the sky half of the hemispherical ambient) |
| Shadow cascades | the light the cascades are fitted to (`fit_shadow_cascades`) |

The fog **density**, the shading mode and everything else in the scene constants stay the renderer's. When the sun is at or below the horizon (`z <= 0`) no cascades are
fitted for that frame: nothing is shadowed by a sun that is not up, and the shadow pass is skipped (the lighting's sun color is what makes the night dark; shadows add
nothing).

### One effective lighting per device frame, at the scene level too

A first `render` of a device frame fixes that frame's effective lighting (the normalized sun, the sun color and the ground ambient), whether or not the view draws models: a sky-only
view publishes nothing to the model renderer, so the model renderer alone could not tell that a later model view of the same frame brings different lighting. Another `render` in
the same device frame (a second view) must supply the *same* lighting (`nullptr` means the construction-time lighting, which counts as a lighting like any other; a scaled
direction is the same sun) or it is refused with `std::logic_error` **before** anything is created, recorded or written. The models' publication happens once per frame, by the first
view that has models; later views with the same lighting do not repeat it. The next frame starts fresh, so a refusal leaves nothing behind. The clock that chooses the lighting is
Core's: it publishes one value per simulation snapshot, and gameplay never writes a renderer slot.

### `time_of_day_lighting`

`time_of_day_lighting(atmosphere, to_sun, day, night_ambient = 0.08)` is pure and deterministic:

* direction: `to_sun`, normalized;
* sun color: `day.sun_color` times `sun_transmittance(atmosphere, to_sun)`: reddening and dimming toward the horizon, exactly zero below it;
* ground ambient: `day.ambient_ground` times `night_ambient + (1 - night_ambient) * smoothstep(sin(elevation))` over sin(elevation) from -0.1 to 0.35, so it fades
  smoothly into the night floor and never rises as the sun sets.

It is a convenience, not part of the contract: a host may build any `SceneLighting` itself.

## The frame-slot contract (`ModelRenderer`)

With `ModelRenderSettings::dynamic_lighting` the renderer keeps one scene-constants buffer (80 bytes) per frame in flight, indexed by `device.frame_number() %
DeviceInfo::frames_in_flight`, each in its own bind group (with the shadow block of the same slot when shadows are on). The frame that owns a slot rewrites it, so the GPU
never reads a buffer the CPU is writing, and a frame still in flight keeps the lighting it was recorded with.

`ModelRenderer::set_lighting_frame(const ModelLightingFrame*)`:

1. throws `std::logic_error` if dynamic lighting is off;
2. throws `std::logic_error` if this frame's slot is already written: by an earlier publication (a second view would overwrite the first view's lighting; several views
   per frame need independent renderers, as for shadows) or by a draw, which completes an unpublished slot and so counts as the frame's write (publication after the
   first draw);
3. throws `std::invalid_argument` for a bad payload (non-finite, negative colors, zero direction, bad sky color);
4. only then writes the slot. `nullptr` publishes the construction-time lighting.

All three checks run before the first write. A frame whose draws find nothing published completes the slot itself: the construction-time lighting, and "no shadows" if
the shadow frame was not set either (a later `set_shadow_frame` in that frame is then refused too), so the draw never reads a slot written for another frame. `SceneRenderer` publishes for its own frame whenever the frame has models;
a host that publishes to the model renderer by hand while also rendering models through `SceneRenderer` in the same frame gets the second-publication error.

`frame_sun_direction()` is the frame's published (normalized) sun, or the construction-time one.

## Limits

* The atmosphere's physical parameters (`AtmosphereSettings`) are still fixed at construction; the time of day moves the sun, not the planet. A host that wants a different
  air (haze, altitude) rebuilds the renderer.
* No moon, stars or sun disc yet, so a night sky is black and the ambient floor is a plain fraction. Clouds and multiple scattering are not modeled (see atmosphere.md).
* Image-based lighting (an equirect environment, #85) is a static environment: it does not follow the sun. A host that wants a changing environment supplies new
  `EnvironmentLighting` explicitly; keeping the two consistent is the host's choice for now.
* Everything here is software-Vulkan correctness evidence. No physical-GPU timing is claimed.

## Verification

* `engine/render/tests/test_scene_renderer.cpp` (null device): off by default and refused when off; the ring's size and release; each frame writes its own slot and a reclaimed
  slot is rewritten, with the other frame in flight untouched; an unpublished frame carries the construction-time lighting; second publication and publication after a
  draw throw with the slot byte-identical afterwards; every kind of invalid payload is refused before any write and does not consume the publication; the sky color
  and the sun in the block follow the frame's sun (the horizon oracle); a sun below the horizon fits no cascades; `time_of_day_lighting` monotonicity, reddening, night
  floor, determinism and refusals.
* `engine/rhi_vulkan/tests/test_dynamic_lighting.cpp` (validated Vulkan, lavapipe in CI): seven suns in a row (more frames than frame slots) against a CPU evaluation of
  the ground; six suns against an independent ray cast of a column's shadow (hundreds of shadowed and thousands of lit probes, none wrong, and the shadow moves); the sky
  pass against `sky_radiance` and the ground's ambient against the horizon color for four consecutive suns; and byte-identical images between the fixed-lighting renderer,
  a dynamic renderer given no lighting, and a dynamic renderer given the same lighting explicitly (with cascades, atmosphere and fog on and off), with a different sun
  being a different image and the next unlit frame returning to the original.

## Per-frame exposure and a night ambient floor (Showcase 03)

Two small additions let a host adapt to the light over a long day without rebuilding the renderer. Both default to the old behavior, so every existing image is unchanged.

* `SceneFrame::exposure_ev` (`std::optional<float>`, HDR only, not retained): replaces `RendererSettings::tone_map.exposure_ev` for that frame's tone map. A host that advances a clock raises the exposure as
  the sun sets (an eye adapting), instead of scaling every light by hand. Bloom and the sky are unaffected apart from the tone map's scale.
* `SceneLighting::ambient_sky_floor` (a non-negative float, default 0): the sky half of the hemispherical ambient becomes `max(fog color * 0.6, ambient_ground * ambient_sky_floor)`. With the atmosphere on,
  the fog color is the horizon radiance of the sky, which falls to nothing at night, so the sky half of the ambient (and every upward-facing surface) goes black while the ground half does not. A host raises
  the floor at night, together with a blue `ambient_ground`, to keep a faint moonlit fill. `time_of_day_lighting` passes the day value through. The scene constants carry it in the previously unused
  `w` of the ambient vector (the shader's `sc_ambient_ground.w`); the second-view lighting check still compares the sun, its color and the ground ambient only.
