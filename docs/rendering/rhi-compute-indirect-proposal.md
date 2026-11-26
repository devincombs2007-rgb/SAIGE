# RHI proposal: compute pipelines, dispatch, indirect draws and storage images

Status (2026-11-26): **approved by the project owner for implementation, subject to compatibility and regression testing** (priority: compute pipelines, dispatch, storage images and indirect drawing, then renderer-level instancing and GPU-driven culling).
Implemented on `claude/fidelity-dev`: step 1 (stage visibility, `StorageTexture`, per-stage totals; commit `dd95c66`) and step 2 (`ComputePipelineDesc`, `create_compute_pipeline`, `dispatch`, `dispatch_indirect`, `DeviceInfo::compute`; null device and Vulkan backend, with lavapipe correctness tests: a fill compared with the CPU, a GPU-written indirect dispatch,
a storage-image write, misuse rejection). Not yet implemented: indirect draws and their capability bits (step 3), then instancing and GPU culling. Two details differ from the sketch below: the dispatch limit is a named constant (`max_dispatch_groups_per_axis`, 65535) checked by both backends, and the storage-image test
uses RGBA32F because the shader toolchain (glslang HLSL) maps `RWTexture2D<unorm float4>` to an `Rgba32f` image and the backend does not enable `shaderStorageImageWriteWithoutFormat`.

Original status line follows. It is additive: no existing signature, default or behaviour changes. `engine/rhi` is graphics architecture (Claude Code's area in `AGENTS.md`); Core consumes
it through `render_sink` and `include/saige` and is not affected by an additive change, but the item is posted for visibility because every later GPU-driven feature depends on it.

## What the RHI already has (verified in `engine/rhi/include/saige/rhi`)

* Vocabulary that anticipates this work: `ShaderStage::Compute`, `QueueType::{Graphics, Compute, Transfer}`, `PipelineStage::{ComputeShader, DrawIndirect, ...}`, `Access::{IndirectCommandRead, ShaderStorageRead, ShaderStorageWrite}`, `BufferUsage::{Storage, Indirect, DeviceAddress, ...}`,
  `BindingType::StorageBuffer`, barriers with `TextureLayout::General`.
* Instanced drawing: `CommandList::draw_indexed(index_count, instance_count, first_index, vertex_offset, first_instance)`. There is **no per-instance vertex binding** (one interleaved per-vertex binding), but a vertex shader can read per-instance data from a `StorageBuffer` indexed by
  `SV_InstanceID`, which the current bind groups already allow. Instancing therefore needs renderer work (a `ModelDraw` that carries an instance range and a storage buffer), **not** an RHI change.
* GPU timestamps: `CommandList::write_timestamp`, `Device::completed_timestamps`, `DeviceInfo::gpu_timestamps`; `SceneRenderer` already writes slots around the scene pass. Per-pass timing needs more slots in the renderer, not RHI work. (An earlier version of `fidelity-plan.md` listed this as an RHI need; that was wrong.)

## What is missing

1. No compute pipeline creation (`Device` only has `create_graphics_pipeline`) and no `dispatch`.
2. No indirect draw or dispatch commands, although the `Indirect` usage and the barrier stage/access exist.
3. Bindings are "visible to the vertex and pixel stages" only; push constants likewise. A compute shader cannot see a bind group today.
4. No storage-image binding (`BindingType` has sampled texture, sampler, uniform buffer and storage buffer only), so a compute pass cannot write a texture.
5. No capability bits for compute, indirect-count draws or storage-image formats.

## Proposed additions

```cpp
// resource_desc.hpp / device.hpp, all additive
enum class StageFlags : uint8_t { None = 0, Vertex = 1, Pixel = 2, Compute = 4 };      // flag enum like the others
struct BindGroupLayoutEntry { /* existing fields */  StageFlags visibility = StageFlags::Vertex | StageFlags::Pixel; };  // default keeps today's behaviour
enum class BindingType : uint8_t { /* existing */, StorageTexture };                       // new value appended; carries a Format and read/write access in the entry

struct ComputePipelineDesc {
    ShaderHandle compute_shader;                       // ShaderStage::Compute
    uint32_t push_constant_size = 0;                   // <= 128, visible to the compute stage
    std::vector<BindGroupLayoutHandle> bind_group_layouts;
    const char* debug_name = nullptr;
};
// Device
virtual PipelineHandle create_compute_pipeline(const ComputePipelineDesc&) = 0;
// CommandList (outside a rendering scope)
virtual void dispatch(uint32_t x, uint32_t y, uint32_t z) = 0;
virtual void dispatch_indirect(BufferHandle args, uint64_t offset) = 0;                    // 3 x uint32
// CommandList (inside a rendering scope)
virtual void draw_indirect(BufferHandle args, uint64_t offset, uint32_t draw_count, uint32_t stride) = 0;
virtual void draw_indexed_indirect(BufferHandle args, uint64_t offset, uint32_t draw_count, uint32_t stride) = 0;
virtual void draw_indexed_indirect_count(BufferHandle args, uint64_t offset, BufferHandle count, uint64_t count_offset, uint32_t max_draws, uint32_t stride) = 0;  // capability-gated
// DeviceInfo
bool compute = false;              bool draw_indirect_count = false;       bool storage_image_rgba8 = false; /* then the formats actually supported */
```

Validation (the null device and the Vulkan backend share it, in the way `validate(const BufferDesc&)` does today): a dispatch needs a bound compute pipeline and compatible groups at every set; an indirect buffer needs `BufferUsage::Indirect` and a range inside it; the count buffer
likewise; `draw_indexed_indirect_count` is refused unless `DeviceInfo::draw_indirect_count`; a layout entry visible to `Compute` is refused on a device without `compute`; a `StorageTexture` needs `TextureUsage::Storage` (already defined) and a supported format.

Synchronisation: the existing barrier vocabulary is sufficient (`ComputeShader` stage, storage read/write, `IndirectCommandRead` at `DrawIndirect`, `TextureLayout::General`); the render graph's pass declarations gain compute passes that read and write buffers and storage images.
A single graphics-capable queue first; a separate async compute queue (the `QueueType` value exists) is a later, separate decision.

Backends: Vulkan maps to `vkCreateComputePipelines`, `vkCmdDispatch[Indirect]`, `vkCmdDrawIndirect`, `vkCmdDrawIndexedIndirect`, `vkCmdDrawIndexedIndirectCount` (core in 1.2) and descriptor stage flags from `visibility`; the null device records and validates.
Whether lavapipe exposes every optional feature is checked by the capability query on the target, not assumed; a test for a missing feature reports `skipped` (exit 77) rather than passing.

## Tests (each also made to fail by mutation, as with the other modules)

* Null device: every validation rule above, with a refusal test and an acceptance test; dispatch outside/inside a rendering scope; layouts that are valid for graphics stay valid unchanged (regression).
* Vulkan (lavapipe) correctness, not speed: a compute shader writing a known pattern and a parallel reduction compared with the CPU result; a compute pass that frustum-culls a draw list and writes indirect arguments, compared with the CPU culling reference and drawn with `draw_indexed_indirect`;
  a storage-image write read back; the synchronisation-validation layer clean.
* No frame-time or throughput figure from software Vulkan is reported as performance.

## Migration notes for consumers

None required. `BindGroupLayoutEntry::visibility` defaults to today's behaviour; `GraphicsPipelineDesc`, `begin_rendering`, `draw` and `draw_indexed` are unchanged; the two new `Device`/`CommandList` virtuals are implemented by the two in-tree backends (null, Vulkan). `render_sink` and `include/saige` do not touch them.

## First consumers, in order

1. GPU frustum culling and instance compaction for repeated props (trees, grass, parked cars), checked against the CPU culling that exists.
2. Particle simulation and rain/spray draws (`saige::environment` supplies the emitter parameters).
3. Temporal and screen-space passes (TAA resolve, GI/AO upgrade) using storage images.
4. Clustered light assignment for point lights.

## Decision requested

Approval to implement the additions above on `claude/fidelity-dev` in small, separately reviewable commits (types and validation first, null device second, Vulkan third, consumers last), with hosted CI on Linux, ASan/UBSan and Windows MSVC for each. No merge to `main` and no new dependency.
