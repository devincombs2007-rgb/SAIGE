# Sun shadows for imported models (M6, first slice)

Opt-in and off by default. With `RendererSettings::shadows.enabled = false` the renderer uses the shadow-free model shader and acquires
nothing for shadows: no shadow map, comparison sampler, shadow pipeline, per-frame buffers or render-graph pass, and the device needs no
sampled-depth or comparison-sampler support. Every image is exactly what it was before shadows existed (the existing tests and goldens are
the proof). Cascade math: `shadow_cascades.hpp` (#77). Research notes live with #61 and are not part of this slice.

## What it does

One directional shadow map for the sun, for imported models (`ModelRenderer`; the 0.1 cubes and the built-in ground are unaffected).

1. **Light direction.** The normalized `RendererSettings::lighting.sun_direction` (`SceneLighting`, #71). `ModelRenderer::sun_direction()` is
   the one stored value that both the lighting constants and the shadow fit read, so they cannot disagree.
2. **Fit** (`fit_shadow_cascades`, one cascade): an orthographic light projection around the bounding sphere of the camera's view range
   `[near, shadows.distance]`, snapped to whole texels so edges do not shimmer when the camera moves. Reverse-Z like the camera.
3. **Shadow pass** (`SunShadow` in the render graph, before `Scene`): the primitives of materials that cast are drawn depth only into a D32 map
   (`shaders/scene/shadow_depth.hlsl`, no pixel shader) with a rasterizer depth bias.
4. **Lookup** (`sun_visibility`): the receiver is moved along its geometric normal by `normal_offset_texels` shadow texels, projected into the
   map, and compared through a comparison sampler over `(2r+1)^2` taps (percentage-closer filtering). The result multiplies only the sun term,
   in all three shading modes. Outside the map, or beyond its depth range, the sun is not occluded.

## Two shader permutations

`shaders/scene/model.hlsl` is compiled with `SAIGE_SUN_SHADOWS = 0` (its own header, `scene_model`) and, through `model_shadowed.hlsl`,
with `= 1` (`scene_model_shadowed`). The first is the shader as it was: its SPIR-V is unchanged byte for byte, so only its recorded source
hash differs. The second adds set 1 bindings 3 (shadow map), 4 (comparison sampler) and 5 (the frame's shadow block). A `ModelRenderer` picks
the permutation, the matching set 1 layout and the resources at construction, from `ModelRenderSettings::shadows.enabled`.

## Casters: opaque and alpha-masked

Only materials with `MaterialData::casts_sun_shadow` cast, and staging sets it from the *authored* glTF alpha mode: opaque and MASK
materials cast, BLEND materials do not (even one drawn opaque because blending is off, whatever pipeline the visible pass uses). A hand-built
material defaults to casting. Face culling does not decide casting: the shadow pass draws both sides.

- **Opaque** (and a MASK with cutoff 0, which keeps every fragment): the vertex-only depth shader, no material.
- **MASK with a cutoff above 0** (`GpuMaterial::shadow_alpha_test`): `shadow_depth_mask.hlsl`, a depth-only pass with a pixel stage that
  discards when base color alpha (texture times factor) is below the cutoff. It binds the material's own set 0 group, the one the visible
  pass binds, so the texture, sampler, factor and cutoff are literally the visible pass's. Vertices need position and uv only.
- **BLEND**: no shadow. Partial shadows are a later slice.

## Failure policy and the frame-slot contract

- Settings are validated before anything is created (`std::invalid_argument`). Enabling shadows creates, in order, the shaders, layouts,
  pipelines (including the shadow pipeline), the map, the sampler, the ring buffers and the groups. If any step throws, the constructor releases
  everything created so far (shader, layout and pipeline handles are raw, so it does so explicitly) and rethrows; the cause reaches the caller
  and nothing leaks. The sweep test fails each of the 23 creations in turn. A renderer that already exists is not affected.
- `ModelRenderer::set_shadow_frame` writes the frame's shadow block into a ring of `DeviceInfo::frames_in_flight` buffers indexed by
  `device.frame_number()`. One renderer, one write per frame: a second call in the same frame throws `std::logic_error` before anything is
  modified, because it would overwrite the block the first view's draws read. Several views per frame need independent `ModelRenderer`s.

## Verified

`tests/test_sun_shadows.cpp` (Vulkan, validation required): every interior ground point around a cube matches an independent CPU ray cast
(shadow and lit, including points hugging the base), also with a non-default, non-unit `SceneLighting` sun; lit faces are not darkened by
their own shadow and the same test with the bias and normal offset removed shows real acne; shadows off is image-identical to the
image without them; frames are bit-identical; a moved occluder moves its shadow; the filter radius sets the penumbra width; invalid settings
throw; and an opaque cube, an opaque double-sided cube, a MASK cube and a BLEND cube (drawn opaque) cast, cast, do not cast (flag off) and do not cast; and a fence with a half-transparent texture casts only where the visible pass keeps fragments (no ghost shadow of the transparent half), with the same texture, factor and cutoff (a 0.4 factor under a 0.5 cutoff casts nothing).
`test_scene_renderer.cpp` (null device): shadows off creates fewer resources and schedules no `SunShadow` pass, the acquisition sweep, and the
once-per-frame contract. `test_stage_model.cpp`: staging sets `casts_sun_shadow` from the authored mode. Mutation-checked: the caster flag, a
fixed instead of the lighting sun direction in the fit, the constructor rollback, the once-per-frame guard and an always-on shadow acquisition
each fail a test, as do the lookup, normal offset, bias, filter radius and fitted matrix.

## Limits (be careful what you conclude from the images)

- One cascade: resolution falls with distance (a 2048 map over a 60 m range is about 5 cm per texel at the far end); cascades and blending are the next slice.
- Blended surfaces do not cast. Alpha-masked ones are tested at the shadow-map's resolution with nearest-filtered depth compare, so a fine cutout (thin foliage) can shimmer as the cascade moves; mip selection is the implicit one of the shadow pass's derivatives, not the camera's.
- No contact-hardening or soft-shadow model beyond the fixed PCF radius; no shadows from the 0.1 cubes; one sun only.
- Software Vulkan (lavapipe) has validated correctness only. Nothing here has run on a physical GPU and no performance claim is made: the pass costs one extra draw of every caster plus
  `(2r+1)^2` comparison taps per lit pixel, to be measured on hardware.
