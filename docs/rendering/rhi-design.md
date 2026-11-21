# RHI & Vulkan Backend Design

**Status:** Proposal. Depends on D-01…D-05, D-11 and D-12.

## 1. Principles

- **Thin and explicit.** The RHI mirrors modern explicit APIs (Vulkan 1.3, D3D12, Metal 3) rather than
  emulating legacy state machines. No implicit state tracking inside command lists: barriers come
  from the render graph.
- **Handles, not pointers.** Every GPU object is a 32-bit generational handle (20-bit index, 12-bit generation) into a backend-owned pool.
  Handles are trivially copyable, safe to compare, and stale handles are detected in debug builds.
- **Bindless first.** Shaders access resources through indices into global descriptor heaps, so there is
  no per-draw descriptor set management.
- **No backend types in public headers.** `rhi/*.h` includes only the C++ standard library and core headers.
- **Validation-clean.** All Vulkan tests run with validation layers on, and any error fails the test.

## 2. Vulkan baseline

**Target:** Vulkan **1.3**. That covers current desktop GPUs from NVIDIA (Turing+), AMD (RDNA+) and
Intel (Xe+), Mesa drivers, and lavapipe for CI.

| Required (core 1.3 / features) | Why |
|--------------------------------|-----|
| `dynamicRendering` | No `VkRenderPass`/`VkFramebuffer` objects. Simpler render graph. |
| `synchronization2` | Clear barrier model the render graph emits. |
| `timelineSemaphore` | Frame pacing, cross-queue sync, deferred deletion. |
| `bufferDeviceAddress` | GPU pointers for GPU-driven passes and ray tracing. |
| `descriptorIndexing` (runtime arrays, partially bound, update-after-bind, non-uniform indexing) | Bindless. |
| `maintenance4`, `shaderDrawParameters`, `drawIndirectCount` | GPU-driven indirect rendering. |
| `scalarBlockLayout` | Shared C++/HLSL struct layout without std140 padding pitfalls. |

| Optional (capability-gated) | Unlocks |
|-----------------------------|---------|
| `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`, `VK_KHR_ray_tracing_pipeline` | Phase 5 ray tracing. |
| `VK_EXT_mesh_shader` | Meshlet rendering and virtualized geometry fast path. |
| `VK_EXT_memory_budget`, `VK_EXT_memory_priority` | Streaming budget management. |
| `VK_KHR_fragment_shading_rate` | Variable-rate shading. |
| `VK_EXT_descriptor_buffer` | Possible future descriptor-model upgrade. |
| `VK_NV_device_diagnostic_checkpoints` / `VK_AMD_buffer_marker` | GPU crash breadcrumbs. |

Vulkan 1.4 features (for example push descriptors and dynamic rendering local read) are used
opportunistically when present. The baseline stays at 1.3 for driver reach.

**Libraries:** `volk` (loader), VMA (memory), Vulkan validation layers + `VK_EXT_debug_utils` (names and labels).

## 3. Object model

```
Instance/Adapter enumeration ─► Device ─┬─► Queue (Graphics | Compute | Transfer)
                                        ├─► Buffer, Texture, TextureView, Sampler
                                        ├─► ShaderModule, GraphicsPipeline, ComputePipeline, RayTracingPipeline
                                        ├─► AccelerationStructure (optional)
                                        ├─► Swapchain
                                        ├─► CommandList (per-thread pools, transient per frame)
                                        └─► Fence / TimelineSemaphore
```

## 4. Memory

- VMA for all allocations. Usage hints: `GpuOnly`, `Upload` (CPU→GPU, write-combined), `Readback`.
- **Upload path:** per-frame ring buffers for small dynamic data. A staging allocator plus the transfer
  queue for bulk asset uploads, with ownership transfer to the graphics queue.
- **Transient render-graph memory:** large heaps with placed or aliased resources, reset each frame.
- **Budgets:** query `VK_EXT_memory_budget`. The streaming systems (Phase 4) consume the budget data.
- **Deferred destruction:** `destroy*()` enqueues the object with the current frame's timeline value.
  The object is actually released once the GPU passes that value.

## 5. Descriptors (bindless-first)

- One global descriptor set (set 0), bound once per command list:
  - binding 0: `Texture2D[]` (sampled), binding 1: `RWTexture2D[]` (storage),
  - binding 2: `ByteAddressBuffer[]` / `StructuredBuffer[]` (storage buffers),
  - binding 3: `SamplerState[]`, binding 4: `RaytracingAccelerationStructure[]` (optional).
  - Descriptor counts are capped by device limits, for example 500k textures and 2k samplers.
- When a texture or buffer is created it gets a stable **descriptor index**, which the RHI returns
  through `getBindlessIndex(handle)`. Shaders receive indices through push constants or GPU scene buffers.
- Push constants (≤128 bytes) carry per-dispatch and per-draw parameters.
- A single global pipeline layout makes pipelines layout-compatible and avoids rebinding.

### 4.1 Texture upload and samplers (0.2)

`Device::upload_texture(texture, data)` is the load-time upload path: synchronous, one staging buffer, one
transfer-and-barrier submission that waits for completion. `data` is tightly packed with the mip level outermost, then
array layer, then rows (the order `compute_texture_data_size` already sums), and must match that size exactly. The
texture needs `Sampled | TransferDst`, one sample and a 2D dimension. Afterwards the texture is in
`sampled_texture_state`, so an immutable uploaded texture never needs another barrier before it is sampled.
Only color formats are uploadable: depth/stencil textures need per-aspect copy regions and layouts, which 0.2 does not
need, so `validate_texture_upload` rejects them (shared by every backend). Re-uploading a texture that earlier, possibly
still-executing frames read or wrote is supported: the upload's first transition is a full dependency on all prior work,
with every earlier write as its source access. The validation layer's synchronization validation reports
`SYNC-HAZARD-WRITE-AFTER-READ` if the execution dependency is removed and `SYNC-HAZARD-WRITE-AFTER-WRITE` if the source
access is (rendering into a texture that also has attachment usage, then overwriting it, is the case that shows it).
Shader-read ordering is argued from the same barrier, not shown by a test: the layer version in use does not track
descriptor reads. Uploading is not allowed between `begin_frame()` and
`end_frame()` (it throws `std::logic_error`). It blocks the
calling thread by design: 0.2 loads synchronously on one thread, and an asynchronous transfer-queue path is added only
after upload cost is measured.

`Device::create_sampler` creates immutable samplers (`hash_sampler_desc` lets callers deduplicate). Requested
anisotropy degrades to trilinear on devices without `samplerAnisotropy` instead of failing the load. Destruction of
samplers and textures is deferred until in-flight frames retire, like every other resource. A texture whose usage allows
no shader or attachment access (transfer only) has no image view.

### 4.2 Bind groups (0.2)

Bind groups are plain descriptor sets: no bindless, no extensions, so the software lanes stay representative. A
`BindGroupLayout` lists bindings of type `SampledTexture`, `Sampler`, `UniformBuffer` or `StorageBuffer`. Textures and
samplers are separate bindings, which is HLSL's model (`Texture2D` + `SamplerState`) and what both glslang and DXC emit;
it also lets materials share one sampler. Pipelines list their layouts (position = descriptor set index, at most 4), and
`CommandList::bind_group(set, group)` binds a group made from the same layout. All bindings are visible to the vertex and
pixel stages.

- **Immutable and load-time.** `create_bind_group` writes the descriptors once. Per-draw or per-frame data goes through
  push constants today and an instance buffer next; there is no dynamic-offset or update-after-bind path yet.
- **One rule set.** `validate(layout, entries, resolver)` in `saige::rhi` is shared by every backend: exactly one entry
  per binding, live resources, `Sampled` usage and one sample for textures, `Uniform` / `Storage` usage for buffers,
  256-byte aligned offsets, and a non-empty range inside the buffer that fits the portable per-binding limits (16 KiB
  uniform, 128 MiB storage: the minimums every Vulkan device guarantees; a whole-buffer binding of a larger buffer is
  rejected and needs an explicit size). Violations throw `std::invalid_argument` at creation.
- **Bind and draw misuse is rejected by the backend, not left to the validation layer.** `bind_group` requires a bound
  pipeline, a set the pipeline declares, a live group, and a group created from that pipeline's layout for the set; a
  draw with a pipeline that declares sets requires a compatible group at every one of them. Each check throws
  `std::invalid_argument` before any Vulkan call is made (the null backend reports the same cases as validation errors).
- **Lifetimes.** A group does not own what it references. Destruction of groups, layouts and the resources they point at
  is deferred until in-flight submissions retire, so they can be destroyed in any order right after a frame is
  submitted (tested on Vulkan with validation). A group must not outlive the device. Leaked groups are reported at
  device teardown like other resources.
- **Pooling.** The Vulkan backend allocates sets from growing pools of 256 sets and frees them individually when they
  retire.
- **Null backend.** Records `BindGroup` commands and reports a validation error for a draw without a compatible group at
  every declared set, a layout mismatch, or an undeclared set.

Tests that want hazard detection set `DeviceDesc::enable_synchronization_validation`. The Vulkan backend then enables the
layer's synchronization validation through `VK_EXT_validation_features` in code (environment variables are ignored by
elevated Windows processes) and reports the outcome in `DeviceInfo::synchronization_validation_enabled`.

## 6. Pipelines

- Pipelines are created from compiled SPIR-V blobs produced offline by `saige-shaderc`
  ([shader-pipeline.md](shader-pipeline.md)).
- Graphics pipelines use dynamic rendering: attachment formats are specified at creation, with no
  render pass object. Viewport, scissor and depth bias are always dynamic state.
- **Pipeline cache:** `VkPipelineCache` persisted to disk per device UUID and driver version.
  Pipelines can be precompiled in the background from a recorded PSO list to avoid hitches.
- Pipeline creation is thread-safe and can run on job-system workers.

## 7. Command recording & submission

- `CommandList` wraps `VkCommandBuffer`. Each worker thread and frame-in-flight gets its own command pool.
- Explicit barrier API (`cmd.barrier(std::span<const Barrier>)`) maps to `vkCmdPipelineBarrier2`. The
  render graph is the main caller, and manual use is allowed for low-level code.
- Submission uses `vkQueueSubmit2` with timeline semaphore waits and signals. Frames in flight are
  bounded by a per-frame timeline value.

## 8. Swapchain & presentation

- Swapchain recreation on resize or out-of-date, and FIFO (vsync) or MAILBOX/IMMEDIATE selection.
- HDR output (`HDR10_ST2084`, `scRGB`) is gated on surface support. Planned for Phase 3.
- Frame pacing: wait on the timeline value from frame *N − framesInFlight* before reusing its resources.

## 9. Debugging & robustness

- Debug names and labels on every object and pass, so RenderDoc and Nsight captures are readable.
- Validation layers in debug builds and tests. GPU-assisted validation is opt-in.
- Device-lost handling: breadcrumb markers are written before and after each pass, and the last
  completed pass is reported in the crash log.

## 10. API sketch (illustrative, not final)

```cpp
namespace saige::rhi {

enum class Backend : uint8_t { Null, Vulkan /*, D3D12, Metal */ };
enum class QueueType : uint8_t { Graphics, Compute, Transfer };

template <typename Tag> class Handle;   // 32-bit: [generation:12 | index:20], generation 0 = invalid (implemented: engine/rhi/include/saige/rhi/handle.h)
using BufferHandle   = Handle<struct BufferTag>;
using TextureHandle  = Handle<struct TextureTag>;
using PipelineHandle = Handle<struct PipelineTag>;
using SamplerHandle  = Handle<struct SamplerTag>;

struct DeviceCaps {
    bool rayQuery, rayTracingPipeline, meshShaders, variableRateShading;
    uint32_t maxBindlessTextures, maxBindlessBuffers;
    uint32_t framesInFlight;
};

class Device {
public:
    virtual ~Device() = default;
    virtual const DeviceCaps& caps() const = 0;

    virtual BufferHandle   createBuffer(const BufferDesc&, std::span<const std::byte> initialData = {}) = 0;
    virtual TextureHandle  createTexture(const TextureDesc&) = 0;
    virtual SamplerHandle  createSampler(const SamplerDesc&) = 0;
    virtual PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc&) = 0;
    virtual PipelineHandle createComputePipeline(const ComputePipelineDesc&) = 0;
    virtual void destroy(BufferHandle) = 0;   // deferred until GPU completes
    virtual void destroy(TextureHandle) = 0;
    // ...

    virtual uint32_t bindlessIndex(TextureHandle) const = 0;
    virtual uint32_t bindlessIndex(BufferHandle) const = 0;

    virtual CommandList& beginCommandList(QueueType) = 0;      // per-thread, per-frame
    virtual void submit(QueueType, std::span<CommandList* const>, const SubmitSync&) = 0;

    virtual Result<SwapchainHandle> createSwapchain(const SwapchainDesc&) = 0;
    virtual FrameContext beginFrame(SwapchainHandle) = 0;
    virtual void         present(SwapchainHandle) = 0;
};

Result<std::unique_ptr<Device>> createDevice(const DeviceDesc&);  // picks the backend from DeviceDesc::backend

} // namespace saige::rhi
```

> **Note on virtuals:** `Device` is called a few hundred times per frame, so virtual dispatch costs
> nothing there. `CommandList` is the hot path. It will be either a concrete class per backend,
> selected at compile time, or a command-stream encoder. The choice will be made from Phase 1 profiling.

## 11. Null backend

`rhi_null` implements the full interface with CPU-side bookkeeping: handle pools, descriptor index
allocation, and validation of usage flags and lifetimes. It records commands into an inspectable log.
The render graph and scene tests use it to assert barriers and pass order without a GPU.

## 12. Future backends

D3D12 maps almost 1:1: descriptor heaps for bindless, enhanced barriers or resource states, and the
same HLSL compiled to DXIL. Metal is reachable through MoltenVK in the short term, and a native Metal
backend could follow if macOS or iOS becomes a target.
