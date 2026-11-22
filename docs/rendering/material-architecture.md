# Material and lighting boundary for PBR (proposal, 0.3)

Status: proposal for review, no implementation. #43 asks for the material and diagnostics boundary to be decided before any
HDR, IBL, cascaded-shadow or new RHI work, and for 0.2 to stay unchanged meanwhile. This document is that decision, built on
what exists on main after 0.2, and written so the later phases it must not block (ray tracing, volumetric atmosphere,
virtualized geometry, a visibility-buffer renderer, a material editor) stay possible.

## 1. What exists

| Layer | Today (0.2) | Where |
|---|---|---|
| CPU material | glTF metallic-roughness set: base color, metallic, roughness, emissive, normal scale, occlusion strength, alpha mode and cutoff, double-sided, five texture slots (all parsed) | `gfx::Material` in `gltf.hpp` |
| GPU material | one bind group per material; **only base color is sampled**; a 32-byte uniform block (base color factor, emissive, alpha cutoff) | `GpuMaterial` in `model.hpp`, `scene/model.hlsl` |
| Pipelines | four variants: front face CCW/CW (mirrored instances) times cull back/none | `ModelRenderer::pipeline_for(mirrored, double_sided)` |
| Lighting | one directional sun, hemispherical ambient and fog, evaluated directly in the pixel shader against an 8-bit sRGB target | `model.hlsl` |
| Diagnostics | versioned JSON: adapter, scene-pass GPU time, device memory, registry totals | `renderer_frame_sink.hpp`, `diagnostics.md` |

The importer already carries every parameter a metallic-roughness material needs, so the first PBR milestones need no new
asset format. The other glTF texture slots currently raise importer warnings and are the work to do.

## 2. The decision: three separable contracts

Material, surface and lighting are kept apart, because each changes for different reasons and the later phases swap one
without touching the others.

1. **`MaterialData` (what an asset says).** A flat, versionable parameter block plus texture slots, one per material. It is
   data, not code. It is created at staging time, owned by the registry as a bind group (as now), and is the only thing the
   asset side and the project format know about.
2. **`SurfaceData` (what a point on the surface is).** The output of evaluating a material at a pixel or hit: base color,
   metallic, roughness, shading normal, occlusion, emissive, alpha, and the geometric normal. One HLSL function,
   `evaluate_surface(MaterialData, interpolants) -> SurfaceData`, in a shared include. A forward pixel shader, a
   visibility-buffer material resolve and a ray-hit shader all call the same function.
3. **Lighting (how a surface responds to light).** Functions of `SurfaceData`, view, and a light or environment: the BRDF
   (GGX, height-correlated Smith, Schlick), punctual and directional lights, image-based lighting, shadows. It never reads
   a texture slot or a material parameter directly.

Consequences that matter later: adding clear-coat or sheen extends `MaterialData` and `SurfaceData` without touching the
lights; adding ray-traced reflections adds a consumer of `SurfaceData`, not a new material path; a node-graph editor produces
`MaterialData` or a new template, not shader forks.

**Not decided here (explicitly out of scope):** bindless material indexing, the visibility-buffer renderer, a material
template system with user-authored shaders. The data layout below is chosen so each can be adopted without changing the asset
side.

## 3. Data layout

- **Texture slots (fixed for the v1 template):** base color (sRGB), metallic-roughness (linear, G roughness, B metallic),
  normal (linear), occlusion (linear, R), emissive (sRGB). Color space is a property of the *slot*, set once; the existing mip
  generator already takes a `ColorSpace`, so mips of linear data are not averaged in sRGB. A missing texture binds a 1x1
  neutral default (white; normal = flat), so the shader has no per-slot branches.
- **Sampler:** one per material from the glTF sampler (filter and wrap), shared through the registry as today.
- **Parameter block:** `float4 base_color; float4 emissive_cutoff; float4 mr_normal_occlusion (metallic, roughness, normal
  scale, occlusion strength); uint flags`. Scalar block layout (`VK_EXT_scalar_block_layout`, core since 1.2, so available on
  the 1.3 baseline wherever the feature bit is set; probed by the capability work) lets the HLSL and C++ structs match without
  padding. Where the bit is missing, the same block is declared with explicit 16-byte alignment: the layout is a build-time
  choice of the shader, not a branch in the renderer.
- **Alpha and sidedness select pipeline state, not parameters:** the existing mirrored/cull variants extend to a key of
  (alpha mode, double-sided, mirrored, color format, depth format). Blend gets its own variant and a sort policy; it is no
  longer drawn opaque.
- **Tangents:** use the asset's tangents when present, otherwise generate them in the asset pipeline (CPU, deterministic,
  tested), never in the shader. The generator (branch `claude/pbr-tangent-generation`) is Lengyel-style: per-triangle,
  angle-weighted, orthogonalized, with the handedness sign in w. It is **not** MikkTSpace, which the glTF specification names
  as the reference, so it can differ at UV seams. Pinning MikkTSpace itself (zlib licence) is a separate dependency decision and
  is not assumed here. (An earlier draft of this document said MikkTSpace; that was wrong about what was implemented.) Vertex format grows from 32 to 48 bytes (adds a float4 tangent); this is the one
  change to a persisted-in-memory layout, and it is isolated to `ModelVertex` and the staging code.
- **Project format:** nothing changes. Materials stay in the glTF. If the project later overrides a parameter, that is a Core
  schema change proposed on its own (see decision 3 below).

## 4. Output and color pipeline

The scene renders in linear light into a floating-point target (`RGBA16_FLOAT`, already in the RHI format table), and a single
tone-mapping pass writes the sRGB swapchain or readback target. Exposure is a renderer setting. The 0.2 path (sRGB target, no
tone map) stays selectable until the HDR path has matching tests, so 0.2 regressions keep their meaning. The tone-mapping
operator is one documented choice (decision 2 below), not a user option in the first milestone.

## 5. Diagnostics boundary

Diagnostics report what the renderer and device can measure, as today, and grow by *sections*, so older consumers keep
working: `schema_version` stays an integer and only increases with a documented change.

| Addition | Source of truth | Reported as |
|---|---|---|
| `materials`: live count, distinct pipeline variants created, material bind-group bytes, textures by slot kind | registry and `ModelRenderer` | counts (exact) and estimated bytes |
| `passes`: GPU duration per pass (scene, tone map, later shadow cascades) | timestamps, extending the existing indices | milliseconds, `null` where unsupported |
| `api`: loader, instance and effective Vulkan version; probed capabilities and their source | `DeviceInfo` (capability probe PR) | strings, so a 1.3 and a 1.4 run are comparable |
| `lighting`: light and shadow-map counts, environment resolution | the lighting pass | counts |

Rules carried over: a value the device cannot measure is `null`, never zero; a software adapter is named so its timings are
not read as GPU evidence; physical-GPU numbers stay in #9. The renderer does **not** compute image-quality metrics; those are
test assertions (below).

## 6. Staged milestones

Each is a separately reviewable change with its own tests and no dependency on a later one.

| # | Change | New code | Verification |
|---|---|---|---|
| M1 | **Material data and default textures.** `MaterialData` block, five-slot bind group, neutral defaults, all slots sampled but shaded as today | `model.hlsl`, `gfx_scene/stage_model`, registry (no new primitive) | all 0.2 pixel tests unchanged and byte-identical where the material is the same; new test that each slot reaches the shader |
| M2 | **Metallic-roughness shading.** `SurfaceData`, the BRDF, sun plus ambient, still LDR | shared HLSL include, a CPU reference of the BRDF in `gfx_test_support` | CPU-oracle comparison on constant-color scenes; white-furnace energy test within a stated tolerance; mutation checks on Fresnel, roughness and Smith terms |
| M3 | **Normal maps and tangents.** Tangent generation in the asset pipeline (done as a separate change, `generate_tangents`); 48-byte vertex and shading remain | importer, `ModelVertex`, staging | a tangent test on a known mesh; a normal-map fixture rendered against the CPU oracle; mirrored instances keep correct handedness |
| M4 | **HDR target and tone mapping.** `RGBA16_FLOAT` scene target, one tone-map pass | render graph pass | the HDR path matches the LDR path where tone mapping is the identity-equivalent; a bright-emissive test that clips in LDR and not in HDR |
| M5 | **Image-based lighting.** Prefiltered environment and BRDF lookup | cube textures, compute or fragment prefilter (RHI additions) | furnace test with the environment; reference images with stated tolerance and provenance |
| M6 | **Cascaded sun shadows** | depth-only and array targets | shadow-acne and peter-panning fixtures; validation clean |

M1 and M2 need no RHI additions. M4 uses an existing format. Cube textures, storage images and compute (M5) are the first RHI
growth, and each is additive. Ray tracing, clouds and virtualized geometry plug in behind `SurfaceData` and the diagnostics
sections above; nothing in M1 to M6 closes them off.

## 7. Interfaces for ray tracing and temporal reconstruction

Neither is implemented in 0.3. The point is that nothing built in M1 to M6 has to be undone to add them. Concretely:

**Ray tracing** (hardware ray queries in a later phase) needs three things that are cheap to keep available now:
- *Geometry readable from a shader*: vertex and index buffers created with storage and device-address usage (buffer device
  address is core since Vulkan 1.2, probed by the capability work), instead of being vertex-input-only as they are today.
- *Material access by index*: `evaluate_surface` takes a `MaterialData` and interpolants, so a ray-hit shader and a raster
  shader share it. Moving materials from per-material bind groups to a global array (bindless) is a contained change behind that
  function; it must not change `MaterialData`.
- *Stable instance and primitive identity*: a draw or a ray hit should be able to report (instance id, primitive id, material
  index). Today a draw knows its model and push constants; adding an instance index to the data a shader can read is additive.

**Temporal reconstruction** (TAA, upscaling, frame generation, ray-tracing denoisers) consumes the previous frame, so the
renderer must be given it; this is the one interface that crosses into Core:
- *Previous-frame world matrix per instance and previous camera pose* in the frame snapshot. `FrameView` carries the current
  frame only. Without both, per-pixel motion vectors cannot be computed correctly for moving objects or a moving camera, and any
  temporal technique falls back to camera-only reprojection (wrong for dynamic objects). Core owns `FrameView` and the world
  matrices; this is a request, not a change made here (see issue #17).
- *Sub-pixel projection jitter* applied by the renderer to its own projection (a renderer-internal parameter, no Core change),
  with the un-jittered matrices kept for motion vectors.
- *A motion-vector and depth output* from the scene pass, and *history resources* with explicit lifetime in the render graph
  (a resource that survives a frame boundary, and is reset on resize or scene replacement). The render graph already owns
  transient resources; history is an extension of it.
- *Determinism*: every temporal input (jitter sequence, frame index) must be reproducible from the frame number, so the
  existing byte-exact capture tests remain possible with jitter off, and meaningful with it on.

None of these touch `MaterialData` or `SurfaceData`; they extend the frame data, the render graph and the buffers' usage flags.

## 8. Decisions needed

1. **Owner and Core:** approve the three-contract split and the M1 to M6 order, or reorder (for example HDR before
   normal maps).
2. **Owner:** which tone-mapping operator is the documented default (the choice is visible in every capture).
3. **Core:** whether the project format may later carry per-instance material overrides (it would be a versioned schema
   addition owned by Core; none is assumed here).
4. **Review:** that Blend materials move from opaque to sorted blending in this track, which changes pixels only for assets that
   use Blend; the 0.2 fixture does not declare an alpha mode, so it is unaffected.
5. **Process:** physical-GPU measurement (#9) before any performance target is set for M2 to M6.
6. **Core:** whether `FrameView` can carry the previous frame's world matrix per instance and the previous camera pose (section
   7). It is not needed for 0.3's milestones, but it is the earliest point where deferring it starts to cost design freedom.
