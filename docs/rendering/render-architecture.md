# Render Architecture

**Status:** Proposal. Depends on decisions D-06, D-08, D-09 and D-15 in
[core-interface-requirements.md](core-interface-requirements.md).

## 1. Goals

1. **AAA visual ceiling.** The architecture must not block physically based lighting, ray tracing,
   virtualized geometry, volumetric atmosphere and dense open worlds later on.
2. **GPU-driven by default.** Culling, LOD selection and draw generation move to the GPU as early as
   Phase 3, which keeps CPU cost independent of scene density.
3. **Explicit and data-oriented.** Thin abstractions over modern explicit APIs. No hidden state,
   no per-draw virtual calls in hot paths.
4. **Backend-agnostic upper layers.** Only `rhi_vulkan` knows Vulkan exists, so a D3D12 (or Metal)
   backend can be added later without touching the renderer.
5. **Testable without a GPU.** The render graph, scene management and asset code all run against the
   null backend.

## 2. Layering

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Core (Codex): ECS, game loop, platform, jobs, assets registry, VFS       │
└───────────────▲───────────────────────────────────────▲──────────────────┘
                │ RenderScene API (extraction)           │ IAssetImporter/Cooker/Loader
┌───────────────┴──────────────────────────┐  ┌──────────┴───────────────────┐
│ saige::render                            │  │ saige::gfx_assets            │
│  Renderer · RenderScene · Views          │  │  glTF import · texture cook  │
│  Feature passes (GBuffer, lights, sky…)  │  │  mesh cook · material cook   │
│  Material runtime · GPU scene buffers    │  │  runtime GPU loaders         │
│  Render Graph (passes, resources, sync)  │  └──────────┬───────────────────┘
└───────────────┬──────────────────────────┘             │
                │ saige::rhi (backend-agnostic)          │
┌───────────────┴────────────────────────────────────────┴─────────────────┐
│ saige::rhi    Device · Buffers · Textures · Pipelines · CommandList ·    │
│               Queues · Swapchain · Bindless descriptors · Barriers       │
├──────────────────────────────┬──────────────────────┬────────────────────┤
│ rhi_vulkan (Vulkan 1.3)      │ rhi_null (tests/CI)  │ rhi_d3d12 (future) │
└──────────────────────────────┴──────────────────────┴────────────────────┘
```

## 3. Frame flow (pending D-08)

```
Game thread   | sim N   | extract N |  sim N+1  | extract N+1 |
Render side   |         | build N-1 |  submit   |   build N   |  submit
GPU           |   frame N-2  |    frame N-1     |    frame N      |
```

1. **Extraction (game thread, short).** Core's ECS systems push changes into `RenderScene`
   (create, destroy, transform, light and camera updates). The changes are recorded into a command
   buffer, not applied directly.
2. **Scene update (render side).** Queued scene commands are applied. Dirty instances are
   uploaded to GPU scene buffers (instance transforms, bounds, material indices) with a ring
   allocator.
3. **View setup.** Per-view data (camera, frustum, previous frame matrices for TAA and motion vectors).
4. **Render graph build & compile.** Feature passes declare their resources. The graph culls unused
   passes, computes barriers, and aliases transient memory.
5. **Execute.** Passes record command lists in parallel on job-system workers (D-09), then submit on
   the graphics queue, with async compute used where it pays off.
6. **Present.** The swapchain is presented, and frame resources are recycled when the timeline
   semaphore signals that the GPU is done with them.

## 4. Render graph

A frame is described each frame as a DAG of passes over virtual resources.

```cpp
graph.addPass("GBuffer", QueueType::Graphics,
    [&](PassBuilder& b) {
        gbufA = b.createTexture("GBufferA", {.format = Format::RGBA8_UNORM, .size = viewSize});
        depth = b.createTexture("Depth",    {.format = Format::D32_FLOAT,   .size = viewSize});
        b.writeColor(gbufA);
        b.writeDepth(depth);
        b.readBuffer(gpuScene.instances, Access::ShaderRead);
    },
    [=](PassContext& ctx, rhi::CommandList& cmd) { /* draws */ });
```

- **Compile:** topological sort, cull passes whose outputs are never consumed, derive
  `VkImageMemoryBarrier2`/`VkBufferMemoryBarrier2` (sync2) from the declared accesses, and batch
  barriers per pass boundary.
- **Transient aliasing:** non-overlapping transient resources share memory heaps. This is the big
  memory win for G-buffers, post-process chains and shadow atlases.
- **Imported resources:** the swapchain image, persistent history buffers (TAA, GI caches) and the GPU
  scene buffers enter the graph as imported resources with known initial and final states.
- **Async compute:** passes can target the compute queue. The graph inserts cross-queue timeline
  semaphore waits.
- **Debugging:** dump the graph to Graphviz, and name every resource and pass for RenderDoc.

**v1 implementation notes** (`engine/render/src/render_graph.cpp`):
- *Culling:* the roots are passes that write an imported resource or call `setSideEffects()`.
  Data dependencies are read-after-write, plus write-after-write when the later write preserves
  contents (`LoadOp::Load`, partial storage writes). A full overwrite (`Clear`/`DontCare`) breaks the
  dependency, so the earlier writer can be culled.
- *Barriers:* tracked per resource as (layout, last write stages/access, readers since the write).
  Read-after-read needs no barrier. Reads in a new stage chain through earlier readers, so layout
  transitions stay ordered. Discarding writes transition from `Undefined`.
- *Aliasing:* greedy best-fit interval assignment per resource kind (textures with textures, buffers
  with buffers). The first use of an aliased resource waits on the previous occupant's last accesses.
  Sizes come from a callback, so the Vulkan backend can supply exact `vkGetDeviceImageMemoryRequirements` values.
- *Not yet:* async-compute scheduling and cross-queue sync, subresource (per-mip) tracking, split
  barriers, parallel recording. All are planned and none changes the public API.

## 5. GPU scene & materials

- **Bindless everywhere:** every texture, buffer and sampler is accessed by an index into global
  descriptor arrays ([rhi-design.md §5](rhi-design.md#5-descriptors-bindless-first)). Draws push only a
  small root constant (instance offset, view index).
- **GPU scene buffers:** `InstanceData[]` (transform, previous transform, bounds, mesh index, material
  index, flags), `MeshData[]` (vertex/index offsets, LODs, meshlet ranges) and `MaterialData[]`
  (parameters plus texture indices).
- **Unified geometry buffers:** large vertex and index buffers suballocated per mesh, so one bind
  covers every draw (required for GPU-driven rendering and ray tracing).
- **Materials:** a material *template* is shader code plus a parameter layout. A material *instance*
  is parameter values plus texture indices. Instances live in `MaterialData[]` and are indexed from
  the instance data. See [asset-pipeline.md §5](asset-pipeline.md#5-pbr-material-model).

### 5a. Skeletal meshes & GPU skinning (owner decision D-19)

- The animation runtime (a separate subsystem) produces per-instance skinning matrices
  (`modelSpaceJoint × inverseBind`, as `float3x4`) and morph weights, and submits them through
  `RenderScene::setSkinningPose` / `setMorphWeights` during extraction.
- **Compute skinning:** one dispatch per frame skins all visible skinned instances (linear blend
  skinning first, dual-quaternion as an option) and applies morph targets, writing to per-instance
  output vertex buffers.
- Downstream passes treat skinned output exactly like static geometry. The previous frame's skinned
  positions are kept, so motion vectors are correct. Phase 5 refits each instance's BLAS from the
  same output.
- Skinning and morph work is culled on the GPU (bounds expanded by a per-skeleton maximum
  displacement), and LOD can reduce joint influence counts.

## 6. Rendering pipeline evolution

| Stage | Visibility | Lighting | Notes |
|-------|-----------|----------|-------|
| Phase 2 | CPU frustum culling, direct draws | Forward PBR, IBL, cascaded shadow maps | Simplest correct baseline, used to validate materials and assets. |
| Phase 3 | GPU instance and meshlet culling (frustum + two-phase Hi-Z occlusion), indirect draws | **Visibility buffer**, then a deferred material resolve; clustered light lists | Visibility-buffer shading decouples geometry density from shading cost, which suits dense worlds and virtualized geometry. |
| Phase 5 | Same | Hybrid raster + hardware RT (ray queries) | RT shadows, reflections and GI fed by the same GPU scene. |
| Phase 6 | Cluster hierarchy LOD selection on GPU, plus a compute rasterizer for micro-triangles | Same | Virtualized geometry writes into the same visibility buffer. |

## 7. Feature roadmap toward AAA targets

All features are original implementations built from public research (papers, GDC/SIGGRAPH talks).
None copy any engine's proprietary code.

**Phase 3 — Modern core**
- HDR pipeline, physically based camera exposure (EV100), auto-exposure histogram
- Tonemapping (AgX / ACES-fitted, selectable), bloom (dual-filter), color grading LUTs
- TAA with motion vectors, plus an integration point for FSR, DLSS and XeSS upscalers
- Clustered (froxel) light culling for thousands of point and spot lights

**Phase 4 — Atmosphere & environment**
- Physically based sky and aerial perspective (LUT-based single and multiple scattering, after
  Hillaire 2020)
- **Volumetric clouds:** ray-marched, with a weather map, Perlin-Worley base and erosion noise, and
  temporal reprojection (after Schneider's Horizon Zero Dawn talks), lit by the sun and sky with
  multiple-scattering approximations and beer-powder effects, casting shadows on terrain
- Froxel volumetric fog with local light injection
- Terrain (clipmaps or a virtual heightfield), GPU-instanced vegetation with wind
- Texture streaming (mip-level feedback), then virtual texturing for terrain

**Phase 5 — Ray tracing**
- BLAS/TLAS management with incremental TLAS rebuild, BLAS compaction and LOD-aware BLAS selection
- RT shadows (sun and area lights), RT reflections, RT ambient occlusion
- Dynamic diffuse GI: probe-based (DDGI-style irradiance volumes) first, screen-space and
  world-space radiance caching later
- Spatiotemporal denoisers (SVGF-family or ReBLUR-style), plus integration points for vendor denoisers

**Phase 6 — Virtualized geometry & shadows**
- Offline cluster building and a simplification DAG (meshoptimizer-based), with streaming of cluster
  pages
- GPU LOD cut selection, two-pass occlusion culling, a hardware/software raster split
- Virtual shadow maps (paged, cached shadow maps matched to the virtualized geometry)

**Material-specific (GTA-style open world)**
- Car paint (clearcoat, metallic flakes), wet surfaces and puddles driven by weather, layered
  materials and decals, skin (subsurface), hair, glass and transmission, emissive city lighting at night

## 8. Threading and synchronization rules

- `RenderScene` mutators are only valid during the extraction window. The data is double-buffered,
  so game and render sides never touch the same memory.
- Command recording uses per-worker command pools (`workerIndex()` from D-09). No pool is ever shared
  between threads.
- GPU lifetime: resource destruction is deferred until the timeline-semaphore value for the frame
  that last used the resource has passed.

## 9. Open questions (rendering-internal)

- Should async compute be on by default for Phase 3, or opt-in per pass? (Leaning opt-in until profiled.)
- When should a D3D12 backend be built? It isn't needed until Windows-specific features or a console
  port demand it, but RHI design reviews will check that D3D12 would map cleanly.
