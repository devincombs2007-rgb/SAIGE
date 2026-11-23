# Sun shadows for imported models (M6, first slice)

Opt-in and off by default: with `RendererSettings::shadows.enabled = false` nothing is rendered or sampled for shadows and every image is
exactly what it was (the existing tests and goldens are the proof). Research and the longer plan: [m5-m6-research.md](m5-m6-research.md).

## What it does

One directional shadow map for the sun, for imported models (`ModelRenderer`; the 0.1 cubes and the built-in ground are unaffected).

1. **Fit** (`fit_shadow_cascades`, one cascade): an orthographic light projection around the bounding sphere of the camera's view range
   `[near, shadows.distance]`, snapped to whole texels so edges do not shimmer when the camera moves. Reverse-Z like the camera.
2. **Shadow pass** (`SunShadow` in the render graph, before `Scene`): every primitive of every model is drawn depth only into a D32 map
   (`shaders/scene/shadow_depth.hlsl`, no pixel shader) with a rasterizer depth bias.
3. **Lookup** (`sun_visibility` in `shaders/scene/model.hlsl`): the receiver is moved along its geometric normal by `normal_offset_texels` shadow texels, projected into
   the map, and compared through a comparison sampler over `(2r+1)^2` taps (percentage-closer filtering). The result multiplies only the
   sun term, in all three shading modes. Outside the map, or beyond its depth range, the sun is not occluded.

Settings (`SunShadowSettings`): `map_size` (16..8192), `distance`, `caster_margin`, `depth_bias_constant`/`depth_bias_slope` (negative moves
casters away from the sun under reverse-Z), `normal_offset_texels`, `filter_radius`. Invalid values throw before the renderer acquires anything.

Per-frame data lives in a uniform ring of `DeviceInfo::frames_in_flight` buffers indexed by the device's frame number, so the CPU can rewrite
it without racing the GPU. The map always exists (one texel when shadows are off) so the shader's bindings are always valid; the first
frame with models takes it from undefined to a legal sampled state.

## Verified

`tests/test_sun_shadows.cpp` (Vulkan, validation required): every interior ground point around a cube matches an independent CPU ray cast
(shadow and lit, including points hugging the base), lit faces are not darkened by their own shadow, the same test with the bias and normal
offset removed shows real acne (so the check is sensitive), shadows off is byte-identical to the image without them, frames are
bit-identical, a moved occluder moves its shadow, the filter radius sets the penumbra width, and invalid settings throw. Mutation-checked:
disabling the lookup, ignoring the normal offset, the bias, the filter radius or the fitted matrix, and skipping the caster draws each fail a test.

## Limits (be careful what you conclude from the images)

- One cascade: resolution falls with distance (a 2048 map over a 60 m range is about 5 cm per texel at the far end); cascades and blending are the next slice.
- Alpha-masked and blended materials cast as opaque (the shadow pass binds no material). Blended surfaces should not cast; that needs the material's blend flag in the shadow pass.
- No contact-hardening or soft-shadow model beyond the fixed PCF radius; no shadows from the 0.1 cubes; one sun only.
- Software Vulkan (lavapipe) has validated correctness only. Nothing here has run on a physical GPU and no performance claim is made: the pass costs one extra draw of every caster plus
  `(2r+1)^2` comparison taps per lit pixel, to be measured on hardware.
