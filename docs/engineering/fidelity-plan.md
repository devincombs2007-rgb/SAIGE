# Fidelity plan: graphical quality, physical realism and rendering efficiency beyond Showcase 04

Status: plan and audit (2026-11-26), written while Showcase 04 rendered on the same four CPU cores. Branch `claude/fidelity-dev`, an isolated line of work that does not touch the Showcase 04 build. GTA VI is a *visual
quality reference* only: no code, asset, tuning value or material from any game is used or implied. All hardware statements are about software Vulkan (lavapipe/llvmpipe, CPU): nothing here is physical-GPU evidence.

## 1. Audit: what exists, what does not (verified by search of the tree, not from memory)

| Area | Exists | Missing | Evidence |
| --- | --- | --- | --- |
| Lighting | HDR, PBR metallic-roughness, normal maps, 3-4 cascade sun shadows with PCF, atmosphere with sun/moon/stars, per-frame time-of-day lighting, emissive gain, IBL (SH9 diffuse, prefiltered specular, fixed per renderer), SSAO, bloom, FXAA | **Point and spot lights**, global illumination, contact shadows, reflections of the scene, volumetrics, clouds | `docs/rendering/*`, `engine/render`, `shaders/scene`; no `point_light`, `volumetric` or `cloud` in the tree |
| Antialiasing | FXAA | **Temporal AA, motion vectors, camera jitter** | no `taa`, `jitter` or velocity buffer in `engine/render` |
| Materials | metallic-roughness, alpha blend, alpha-tested shadow caster, GPU skinning | clearcoat, transmission, sheen, subsurface, **translucency (foliage)**, wet-surface response in a shader | no `KHR_materials_*` handling in `engine/gfx_assets` |
| Transparency | sorted alpha blending | order-independent transparency, refraction | `docs/rendering/alpha-blending.md` |
| Vegetation | procedural low-poly trees in Showcase 04 only | wind animation, instanced draws in the renderer, translucency, LOD, real tree assets | Showcase 04 renderer; no `wind` in the engine; `ModelDraw` has no instance range (the RHI's `draw_indexed` already takes one) |
| Weather | ambience *audio* takes a rain input (`ambience_mix`); water takes a wind speed (`waves_from_wind`) | rain, wetness, puddles, fog model, storm transitions, rain particles | `include/saige/audio_model.hpp`, `include/saige/water.hpp` |
| Particles | none in the engine (Showcase 04 draws smoke and sparks as simple meshes) | a particle system | no `particle` outside destruction hints |
| Destruction | authored fracture pieces, breakable links, events, persistent damage records (`include/saige/destruction.hpp`) | runtime fracture, deformation | `docs/physics-backends.md` |
| Vehicle damage | numeric damage state with physical consequences | progressive mesh deformation | `docs/vehicle-dynamics.md` |
| Characters | humanoid ragdoll, recovery blend, motors, ankle strategy | stepping recovery, get-up, UAL-rig ragdoll, bracing, object interaction | `docs/ragdoll.md` |
| Water | Gerstner surface, buoyancy, drag, currents, foam indicator, 1-100 body workloads | wakes, spray, shoreline, depth map, scene reflections | `docs/water.md`, backlog item WATER-3 |
| GPU-driven rendering | RHI has `QueueType::Compute`, an `Indirect` buffer usage and indirect/compute barrier stages | **compute pipelines and indirect draw commands in the RHI**, GPU culling | `engine/rhi/include` has no compute pipeline or indirect draw call |
| Streaming | cell streaming and origin migration (Core); prepared-model upload staging; device memory budget reporting | texture streaming, geometry streaming, per-category budgets | `engine/gfx_scene`, `engine/rhi/include/saige/rhi/device.hpp` |
| Ray tracing | an `AccelerationStructureBuild` pipeline-stage enum only | everything: capability query, acceleration structures, ray queries | `engine/rhi_vulkan/src/vulkan_device.cpp` |
| Instrumentation | frame-time zones, perf reports, resource diagnostics, RHI GPU timestamps (`write_timestamp`, `gpu_timestamps`), scene-pass slots written by `SceneRenderer` | timestamp slots for the other passes, streaming counters | Core perf, `docs/rendering/diagnostics.md`, `engine/rhi/include/saige/rhi/device.hpp` |

Nothing in the requested list is already built except the foundations above. There is no duplicated work to avoid in the new items; the overlaps to respect are `ambience_mix` (rain/wind audio), `waves_from_wind` (wind-driven sea),
`FloatingBody::wetted()` (the hook for wakes) and the alpha-tested shadow caster (foliage shadows).

## 2. Dependency order

Each item names what must exist first. Items marked CPU can proceed while a render occupies the machine; items marked GPU need shader work and, for evidence, a Vulkan device (lavapipe is enough for correctness, never for speed).

1. **CPU models of environment behaviour** (done in this branch, `engine/environment`): weather and transitions, wind with travelling gusts, surface wetness and puddles, wet-material response, vegetation bend and flutter, stable grass scatter with
   non-popping LOD thinning, wake foam. They define the numbers the shaders and passes will reproduce. Needs nothing.
2. **RHI: compute pipelines, dispatch, indirect draws, compute-visible bindings and storage images** (GPU; owner of the RHI contract is Graphics; the full additive proposal is `docs/rendering/rhi-compute-indirect-proposal.md`). Everything GPU-driven, particles, temporal and volumetric passes need compute or indirect draws. Additive, so no
   migration for consumers, but proposed in issue #43 first.
3. **Point and spot lights** (GPU): clustered or tiled forward lighting with a light-count cap and the capability gate. Needed by wet-street reflections at night, vehicle headlights that light the road, lit interiors, lightning. Needs 2 only if
   clustering uses compute; a simple forward loop with a budget does not.
4. **Motion vectors and camera jitter, then TAA** (GPU). CPU reference first (item 6 below). Needs depth and a velocity target; skinned and animated draws need previous-frame transforms/palettes, which is an interface change in `ModelDraw`.
5. **Particles** (GPU, with a CPU reference): rain streaks, splashes, spray, smoke, sparks as one system with emitters from the weather, the water module and the vehicle. Needs 2 for GPU simulation; a CPU-simulated instanced draw works first.
6. **Vegetation rendering**: instanced draws (needs `ModelDraw` to carry an instance range and a storage buffer of per-instance data; this is renderer work, not an RHI change), the wind bend in the vertex stage using the CPU reference as the test oracle, two-sided translucency, alpha-tested shadows (exists). Professional tree assets need licence review.
7. **Wet surfaces in shaders**: the `wet_surface` model as a material modifier (needs the weather state as scene constants and a puddle mask source: a height/noise texture or the decal system), reflections of the sky by IBL (exists) and of the scene by screen-space reflections (needs depth and a colour history).
8. **Global illumination**: see section 3. Needs 2 and 4 for any screen-space or probe-update technique.
9. **Volumetrics and clouds**: froxel fog and a ray-marched cloud layer on the existing atmosphere model. Needs 2.
10. **Streaming**: budgets and counters first (CPU, pure accounting), then texture streaming, then geometry streaming. Needs 2 for GPU upload scheduling; budgets do not.
11. **Hardware ray tracing**: capability-gated, last (section 4).

Physical interaction, in parallel and independent of the renderer: (a) breakable environment hosted in Showcase-class worlds (needs Jolt, optional and gated), (b) vehicle deformation as a lattice or per-panel displacement driven by `VehicleImpact`
(an approximation, never claimed as a soft-body solver), (c) ragdoll on the real UAL rig then stepping recovery, (d) water: wakes (foam trail built), spray, shoreline depth, reflections.

## 3. Global illumination: options and recommendation

| Technique | What it gives | Cost and risk | Fit |
| --- | --- | --- | --- |
| Baked/static irradiance probes (SH9 grids), blended by position | cheap, stable diffuse bounce for static districts; reuses the existing SH9 code | static lighting only; needs a bake tool and a probe volume format; leaks through thin walls unless visibility is stored | **first step**: nearly all the machinery exists (`gfx_assets` environment SH) |
| Screen-space GI/AO (GTAO-class, SSDO) | contact shading and short-range colour bleeding | view dependent, noisy without temporal filtering, no off-screen light | upgrade of the existing SSAO once TAA and motion vectors exist |
| Dynamic probe grids with ray-traced or rasterised updates (DDGI-class) | dynamic diffuse GI for moving sun and weather | needs compute, visibility moments, probe relocation, significant memory; hardware RT optional (software tracing possible against a voxel or SDF scene) | second step, after RHI compute |
| Voxel cone tracing | diffuse and glossy GI | large memory, light leaking, update cost, hard to make stable | not recommended |
| Hardware ray-traced GI/reflections | highest fidelity | needs RT hardware and an acceleration-structure RHI; cannot be validated on software Vulkan | capability-gated last step |

Recommendation: static probe grid first (CPU bake, using the existing SH tools), then temporal screen-space GI/AO, then dynamic probes on compute, with hardware RT as an optional high tier behind the capability gate. At no stage is a frame-time or quality number
reported from software Vulkan as a performance result.

## 4. Capability-gated hardware ray tracing

Design only. The backend reports `ray_tracing_pipeline`, `ray_query` and `acceleration_structure` features separately (Vulkan KHR extensions); the renderer exposes `RendererCapabilities::ray_tracing` and a settings enum
`{Off, Reflections, ReflectionsAndShadows, GI}` that the host may request; a request the device cannot meet falls back to the raster path and the fallback is reported, never silent. The null and software adapters report no support and every RT test is
`skipped` (exit 77), not passed. A feature is listed as operational only after a run on an RT-capable physical GPU, recorded with the adapter classification and build hash. None exists today.

## 5. Temporal anti-aliasing: design

A CPU reference first (so the shader port has an oracle): a Halton(2,3) 8-16 sample jitter sequence applied to the projection, motion-vector reprojection, a YCoCg neighbourhood clamp (3x3 min/max, or variance clipping) of the history, an exponential blend (about 0.1 weight for
the current frame, reduced under motion), a sharpening pass, and rejection on disocclusion. Requirements on the engine: a velocity target (animated and skinned geometry need the previous frame's transforms and joint palettes), a history texture per view with a reset on camera cuts, and
mip-bias on texture sampling by log2(render/output). Verification: a static scene converges to the supersampled average of the jitter set; a moving edge does not ghost beyond the clamp; a camera cut resets the history; the offline film path can instead average sub-frames (the open item in the backlog).

## 6. Performance and production infrastructure

* **Instancing and culling**: instance streams (trees, grass, props) before GPU culling; the existing CPU frustum culling already drops draws that cannot reach the screen. Instrument draw counts per pass before changing anything.
* **GPU-driven rendering**: needs RHI compute and indirect draws (item 2). Without a GPU, correctness is testable on lavapipe; speed claims need hardware.
* **Streaming and budgets**: a CPU budget ledger (textures, geometry, per-category caps, eviction by distance and recency) is pure accounting and can be built and tested now; the device memory budget the RHI already reports feeds it.
* **Parallel rendering preparation**: record command buffers per cascade/pass on worker threads once the RHI's thread contract is written down; measure before restructuring (the directive requires representative workloads first).
* **Instrumentation**: more GPU timestamp slots (the RHI already provides timestamps; the renderer writes only the scene pass), draw/triangle counters, and streaming counters, written into the existing perf report format.

## 7. What was built in this branch (and what was not)

`engine/render_reference` (`saige::render_reference`, 7 tests): the TAA reference of section 5: Halton(2, 3) jitter and its NDC offset, texture LOD bias, YCoCg, and `taa_resolve` (reprojection, variance clipping bounded by the neighbourhood min/max, motion-dependent blend,
cut and off-screen handling). Verified by nine deliberate defects, each caught (no history clip, flipped reprojection, biased jitter, off-screen history reused, no accumulation, flipped NDC y, wrong YCoCg inverse, unbounded clip box, motion-blind blend); three tests were too weak at
first (a flat test image let the clip hide an off-screen bug; a huge motion vector saturated the blend; `motion_blend` had no test) and were tightened. The guarantee the tests pin is that the output stays inside the 3x3 neighbourhood's min/max range in YCoCg, not inside the
tighter variance box (an outlier centre pixel can sit outside it). It is a bilinear-history reference: a production resolve would use a Catmull-Rom history filter and a sharpening pass, which are not here.

`engine/residency` (`saige::residency`, 8 tests): the CPU residency ledger of section 6, pure accounting for streamed textures (mips) and geometry (LODs): per-category and total byte budgets, pinned resources, an upload allowance per frame, and a deterministic planner
(evict unrequested resources least recently used first; degrade requested ones one level at a time, least important first, never below the coarsest level while it fits; evict whole requested resources only when even the coarsest levels do not fit; then pace the upgrades by the
allowance, partially if needed). Verified by eleven deliberate defects, each caught after one test was rewritten (the first LRU test had a single eviction candidate, so the order could not matter). It allocates nothing and knows no device: the renderer maps its plans onto real
allocations and the device's reported budget. Not built: a texture streamer, mip upload, geometry streaming, or any integration; no GPU or throughput measurement.

`engine/environment` (below) as before.

`engine/environment` (23 tests, built with `-Werror`, Debug, GCC): see its header comments. Verified by 15 deliberate defects, each caught by at least one test (exact-integration wetness, wrong Kelvin angle, foam that never decays, a speed-independent foam
strength, ignored grass jitter, a region-dependent scatter hash, a sway that does not vanish in still air, rain-blind visibility, rain that jumps at phase changes, popping LOD, a stretching bend, a Clear-to-Storm transition, brightening wet surfaces, puddles below the
drainage rate and out-of-bounds gusts). Not built: any renderer or shader change, any Vulkan code, any asset, any wiring into a showcase. Not measured: GPU cost of anything. Limits worth stating: the rain extinction law is a published empirical fit used out of its calibration range
for the heaviest storm; the sway and flutter amplitudes are tuned for plausibility, not measured on real plants; the foam trail is a visual model, not a wave simulation; the weather schedule is a seeded Markov-like walk, not meteorology.


## Addendum 2026-11-26: owner fidelity requirements (director directive)

The owner asked for substantially higher fidelity throughout SAIGE. These are recorded here in dependency order without displacing P0 correctness work; each item ships with real visual evidence (stills, before/after, short clips) and a stated hardware class, per the Continuous Visual Validation Program.

| Priority | Requirement | Depends on | First measurable slice |
| --- | --- | --- | --- |
| P0 | Owner-reported defects in earlier footage: vehicle hazard lights detached from the body; cafe chairs facing the wrong way; uniform blue window material; too few reflections; too little indirect light; repetitive low-quality assets | World generation, asset validation | Reproduce each in a diagnostic render, then fix at the engine or generator (semantic placement rules for furniture, light-mesh attachment checks, emissive material and transform validation). **None is reproduced yet**; Showcase 04 stays immutable |
| P1 | Physically based glass (Fresnel, roughness, tint, transmission, refraction where practical, interior visibility) | Sorted alpha blending, IBL, scene-colour copy | Glass BRDF CPU reference and GPU probe, then a window test card |
| P1 | Hybrid reflections: probes, screen-space, planar where useful, ray tracing where supported; vehicle paint (clearcoat), wet roads, windows | IBL (done), depth and velocity, glass | Reflection probe capture and parallax-corrected lookup; clearcoat layer in the material model |
| P1 | Interior mapping for inaccessible and distant windows (perspective-correct rooms, varied layouts, furnishings, window depth, night lighting) | Material model, local lights | One-shader interior-mapped facade with room atlas, CPU reference for the ray-box test; real 3D interiors stay for accessible buildings |
| P1 | Global illumination: probe-based first (diffuse bounce, colour bleeding, interior illumination, dynamic response), screen-space GI after velocity exists, hardware ray tracing where supported | Local lights (steps 1 to 3 done), velocity | Irradiance probe volume with CPU reference; see the GI evaluation already in this plan |
| P2 | Surface detail: parallax occlusion mapping, detail normal and albedo, microfacet roughness variation, material layering, wet surfaces, distance-based quality transitions | Material architecture M-steps | Detail normal blend and POM with a tangent-space reference |
| P2 | Shadows and ambient: contact shadows, AO without dark outlines, soft shadows, shadow stability, local-light shadows, indirect occlusion, volumetric lighting | Local lights, cascades (done) | Local-light shadow maps behind the clustered culling |
| P2 | Temporal reconstruction: SSR validation and the integration plan in `docs/rendering/ssr-integration-plan.md` | Velocity, jitter, exposure | R1 and R2 of that plan |
| P2 | Atmospheric scattering (done, `atmosphere.md`), volumetric clouds and fog, HDR exposure and tone mapping, decals, texture streaming, GPU-driven culling (done for instances), hierarchical LOD, impostors, quality scalability | Per item | Volumetric fog on the existing froxel-free path first |
| P3 | Asset library expansion: vehicles, buildings, furniture, vegetation, street infrastructure, everyday objects, characters, clothing, interiors, materials | Asset pipeline (done) | Per-asset validation: scale and orientation, material correctness, texture quality, LOD compatibility, collision, rigs, moving components (lights, doors, wheels), attachment alignment, licence and provenance. Existing authorised free sources only; no bulk downloads |

Avoid cost and licence surprises: every new dependency or source goes through the dependency review in `AGENTS.md`; nothing is purchased.
