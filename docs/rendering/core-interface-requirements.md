# Core ↔ Rendering Interface Requirements

**Audience:** Core / Architecture (ChatGPT/Codex), Project Owner
**Status:** Open. Each item needs a decision from Core (or the owner) before Rendering builds code that depends on it.

Each decision has a stable ID (`D-xx`) so replies can reference it, e.g. "D-06: accepted with Y-up
instead". Rendering's recommendation is given for each, but **Core owns the final call** for
everything outside the graphics subsystem. The code sketches show *what Rendering needs*. They are
not a demand for specific names, and Core is free to reshape them.

Legend: 🔴 blocks Phase 1 · 🟡 blocks Phase 2 · 🟢 needed later

---

## 1. Foundations

### D-01 🔴 Language standard & toolchains
- **Need:** A known C++ standard and supported compilers for writing the RHI and backends.
- **Recommendation:** C++20 (concepts, `std::span`, designated initializers, `<bit>`). Targets:
  MSVC 19.3x (VS 2022), Clang 16+ and GCC 13+. Windows and Linux first. macOS via MoltenVK later.

### D-02 🔴 Build system & target conventions
- **Need:** Somewhere to add `saige::rhi`, `saige::rhi_vulkan`, `saige::render`, `saige::gfx_assets`
  and tool and test targets.
- **Recommendation:** CMake ≥ 3.25 with `CMakePresets.json`. Each module is a static library with an
  alias target `saige::<module>`. Warnings-as-errors in CI. An option
  `SAIGE_RHI_VULKAN=ON|OFF`, so Core can build without the Vulkan SDK installed.

### D-03 🔴 Third-party dependency management
- **Need:** Graphics depends on several third-party libraries (see table).
- **Recommendation:** vcpkg manifest mode (`vcpkg.json`) with a pinned baseline, or CPM/FetchContent
  if Core prefers. The Vulkan SDK (headers, validation layers, `dxc`, `spirv-val`) is a
  developer prerequisite.

| Library | Purpose | License |
|---------|---------|---------|
| Vulkan headers + `volk` | API and function loader | Apache-2.0 / MIT |
| Vulkan Memory Allocator (VMA) | GPU memory suballocation | MIT |
| DirectX Shader Compiler (`dxc`) | HLSL → SPIR-V (later DXIL) | NCSA |
| SPIRV-Reflect | Shader reflection and validation | Apache-2.0 |
| fastgltf | glTF 2.0 parsing (offline tools only) | MIT |
| meshoptimizer | Mesh optimization, meshlets, LOD simplification | MIT |
| MikkTSpace | Tangent generation (required by the glTF spec) | zlib-style |
| KTX-Software / libktx | KTX2 texture container | Apache-2.0 |
| bc7enc_rdo or ISPC Texture Compressor | BCn texture encoding (offline) | MIT / Unlicense |
| stb_image | PNG/JPG decode (offline) | MIT / Public Domain |
| Tracy | CPU/GPU profiling (if adopted by Core, see D-11) | BSD-3 |

### D-04 🔴 Directory layout & module boundaries
- **Need:** Where graphics code lives, and which modules may depend on which.
- **Recommendation:** See [README §4](README.md#4-proposed-module-layout-pending-d-04). Dependency
  rule: `render → rhi → core`, `rhi_vulkan → rhi`, `gfx_assets → core (+ render for runtime loaders)`.
  **Core never depends on rendering headers** except through the narrow interfaces in §2.

### D-05 🔴 Coding conventions
- **Need:** Naming, error handling, exceptions/RTTI policy, formatting.
- **Questions for Core:**
  1. Naming style (e.g., `PascalCase` types, `camelCase` functions, `m_` members)?
  2. Exceptions and RTTI: enabled or disabled? *(Recommendation: disabled in runtime modules. Tools may use them.)*
  3. Error type? *(Recommendation: a `saige::Result<T, E>` / `Expected` type, since `std::expected` is C++23.)*
  4. `.clang-format` / `.clang-tidy`: does Core provide them?

---

## 2. Runtime interfaces Rendering consumes

### D-06 ✅ Coordinate system & units — **DECIDED (owner, 2026-11-21)**
- **Decision:** Right-handed, **Z-up**, 1 unit = 1 meter, as the **engine-wide standard**. See
  [docs/standards/coordinate-system.md](../standards/coordinate-system.md).
- **Still open (Core):** the forward axis. Recommended: +X forward, +Y left.
- **Renderer-internal (no Core decision needed):** reverse-Z depth (`D32_SFLOAT`, near = 1, far = 0,
  infinite far plane), and Vulkan's Y-down clip space handled with a negative-height viewport.

### D-07 🟡 Math library
- **Need:** Vector, matrix and quaternion types that can be `memcpy`'d into GPU buffers.
- **Recommendation:** Column-major storage and column vectors (`v' = M * v`, the glTF/HLSL-`column_major`
  convention). POD types `float2/3/4`, `float3x4`, `float4x4`, `quat`, with a known `alignof`.
  SIMD types are fine internally, as long as packed POD equivalents exist for GPU upload.
- **Question:** Is Core writing its own math library, or adopting one (GLM, DirectXMath, …)?

### D-08 🟡 Threading & frame model
- **Need:** To know which thread calls into the renderer and how frames overlap.
- **Recommendation:**
  - Game/simulation thread produces frame *N*. An **extraction** sync point copies render-relevant
    state into the `RenderScene` (see D-15).
  - Render thread (or render tasks on the job system) builds and submits frame *N−1*.
  - GPU executes up to **2 frames in flight**.
  - Rendering owns everything after extraction.

### D-09 🟡 Job system
- **Need:** Parallel command recording, parallel culling, parallel asset cooking.
- **Minimum API sketch:**
  ```cpp
  namespace saige::jobs {
  JobHandle schedule(Function<void()> fn, std::span<const JobHandle> deps = {});
  JobHandle parallelFor(uint32_t count, uint32_t batch, Function<void(uint32_t begin, uint32_t end)> fn);
  void      wait(JobHandle);
  uint32_t  workerIndex();   // stable per-thread index in [0, workerCount()), for per-thread command pools
  uint32_t  workerCount();
  }
  ```

### D-10 🟢 Memory allocation
- **Need:** CPU-side allocators for render structures (frame-linear and pool allocators). GPU memory
  is managed inside the RHI via VMA.
- **Request:** An allocator interface, or a policy saying "use global new/delete for now".

### D-11 🔴 Logging, asserts, profiling
- **Need:** Shared macros so graphics output goes to the same sinks as Core's.
- **Sketch:** `SAIGE_LOG_{TRACE,INFO,WARN,ERROR}(category, fmt, ...)`, `SAIGE_ASSERT(cond, msg)`,
  `SAIGE_VERIFY(...)`, `SAIGE_PROFILE_SCOPE(name)`.
- **Recommendation:** Tracy for profiling (it has Vulkan GPU zone support). Rendering will register a
  `Render` log category and route Vulkan validation messages to it.

### D-12 🔴 Platform window & surface
- **Need:** A native handle for `VkSurfaceKHR` creation, plus resize, minimize and DPI notifications.
- **Sketch:**
  ```cpp
  namespace saige::platform {
  enum class WindowSystem : uint8_t { Win32, Xlib, Xcb, Wayland, Cocoa, Headless };
  struct NativeWindowHandle {
      WindowSystem system;
      void*        display;  // HINSTANCE | Display* | xcb_connection_t* | wl_display*
      uintptr_t    window;   // HWND | Window | xcb_window_t | wl_surface*
  };
  struct WindowEvents {           // or Core's event bus
      void (*onResize)(void* user, uint32_t widthPx, uint32_t heightPx);
      void (*onMinimizeChanged)(void* user, bool minimized);
      void (*onDpiScaleChanged)(void* user, float scale);
  };
  }
  ```
- **Note:** `Headless` lets tests and CI run offscreen with no window.

### D-13 🟡 File system / VFS
- **Need:** Read cooked assets (preferably asynchronously or memory-mapped), resolve engine paths,
  and watch files in dev builds (shader and asset hot reload).
- **Sketch:** `Result<Blob> readFile(Path)`, `AsyncReadHandle readAsync(Path, Range, Callback)`,
  `MappedFile map(Path)`, `WatchHandle watch(Path, Callback)` (dev builds only).

### D-14 🟡 Asset system ownership
- **Proposal:**
  - **Core owns:** asset registry, stable `AssetId` (recommend a 128-bit GUID stored in sidecar
    metadata, *not* a path hash, so renames don't break references), reference-counted handles,
    the dependency graph, cook orchestration and the streaming I/O scheduler.
  - **Rendering owns:** importers (glTF, PNG/JPG/EXR, HDR), cookers (mesh, texture, material,
    shader) and runtime loaders that turn cooked bytes into GPU resources.
- **Plugin sketch:**
  ```cpp
  namespace saige::assets {
  struct AssetId { uint64_t hi, lo; };

  class IAssetImporter {          // tools/editor only: source file -> one or more source assets
  public:
      virtual ~IAssetImporter() = default;
      virtual std::span<const std::string_view> extensions() const = 0;
      virtual Result<ImportOutput> import(const ImportContext&) = 0;
  };
  class IAssetCooker {            // tools only: source asset -> platform-specific cooked bytes
  public:
      virtual ~IAssetCooker() = default;
      virtual AssetType type() const = 0;
      virtual uint32_t  version() const = 0;     // bump to invalidate the cook cache
      virtual Result<CookOutput> cook(const CookContext&) = 0;
  };
  class IAssetLoader {            // runtime: cooked bytes -> live resource (e.g., GPU mesh)
  public:
      virtual ~IAssetLoader() = default;
      virtual AssetType type() const = 0;
      virtual Result<void*> load(const LoadContext&) = 0;
      virtual void unload(void* resource) = 0;
  };
  }
  ```

### D-15 🟡 ECS ↔ Renderer integration
- **Proposal:** The renderer does **not** read ECS storage directly. It exposes a `RenderScene` API
  with opaque handles. Core's ECS systems (e.g., `MeshRenderSyncSystem`) call it during extraction.
  This keeps the renderer independent of ECS internals, and keeps the ECS independent of GPU types.
  ```cpp
  namespace saige::render {   // owned by Rendering, called by Core
  class RenderScene {
  public:
      InstanceHandle createMeshInstance(const MeshInstanceDesc&);  // mesh AssetId, materials, transform, flags
      void           setTransform(InstanceHandle, const float4x4& localToWorld);
      void           setVisible(InstanceHandle, bool);
      void           destroy(InstanceHandle);

      LightHandle    createLight(const LightDesc&);                // directional / point / spot / area
      void           updateLight(LightHandle, const LightDesc&);
      void           destroy(LightHandle);

      void           setMainView(const ViewDesc&);                 // camera transform, projection, exposure
      void           setEnvironment(const EnvironmentDesc&);       // sun, sky, fog, clouds parameters
  };
  }
  ```
  Calls are only valid during the extraction window (D-08). Internally they're buffered and
  applied on the render side.
- **Question:** Does Core prefer that Rendering define the ECS *components* (`MeshComponent`,
  `LightComponent`, `CameraComponent`), or that Core defines them and Rendering only provides
  `RenderScene`? *(Recommendation: Core defines the components; Rendering provides `RenderScene` and the descriptor structs.)*

### D-16 🟢 Configuration / console variables
- **Need:** Runtime-tweakable render settings (`r.Shadows.Resolution`, `r.VSync`, …).
- **Request:** A CVar or config API, or approval for Rendering to use a local settings struct until one exists.

### D-17 🔴 Test framework & CI
- **Recommendation:** GoogleTest *or* doctest, whichever Core prefers. GitHub Actions on Windows and
  Linux. The Linux job installs Mesa **lavapipe** so Vulkan smoke tests run without a GPU (see
  [testing-strategy.md](testing-strategy.md)).

### D-18 🔴 Subsystem lifecycle
- **Need:** How the renderer is created, initialized, ticked and shut down, and in what order relative
  to platform, assets and jobs.
- **Sketch (Rendering's side):**
  ```cpp
  namespace saige::render {
  struct RendererDesc { platform::NativeWindowHandle window; rhi::Backend backend; bool enableValidation; };
  class Renderer {
  public:
      static Result<std::unique_ptr<Renderer>> create(const RendererDesc&);
      RenderScene& scene();
      void beginExtraction();   // game thread, start of the sync point
      void endExtraction();     // game thread, end of the sync point
      void renderFrame();       // render thread / job
      void onResize(uint32_t w, uint32_t h);
      // destructor waits for the GPU to go idle and releases all resources
  };
  }
  ```
  Required init order: platform → jobs → filesystem/assets → renderer. Shutdown is the reverse.

### D-19 🟡 Animation subsystem boundaries — **OWNERSHIP DECIDED (owner, 2026-11-21)**, contract open
- **Decision:** Animation is an **independent engine subsystem**.
  - **Core (Codex):** animation core interfaces and engine integration.
  - **Rendering (Claude):** skeletal mesh rendering, GPU skinning, graphics-related asset import
    (skeletons, skins, animation clips, morph targets from glTF).
  - **Playback, blending, retargeting and IK:** a separate modular architecture. Ownership must be
    agreed **before** implementation (tracked in its own issue).
- **Proposed data contract (for agreement with Codex):**
  ```cpp
  // Produced by Rendering's importers, consumed by the animation runtime
  struct SkeletonAsset  { /* joint names, parent indices, bind pose (local TRS), inverse bind matrices */ };
  struct AnimClipAsset  { /* per-joint TRS tracks (compressed), duration, sample rate, root motion flags */ };

  // Produced by the animation runtime each frame, consumed by the renderer through RenderScene
  void RenderScene::setSkinningPose(InstanceHandle, std::span<const float3x4> jointSkinningMatrices);
  // jointSkinningMatrices[i] = modelSpaceJoint[i] * inverseBind[i] (model space, in the D-06 convention)
  void RenderScene::setMorphWeights(InstanceHandle, std::span<const float> weights);
  ```
- **Rendering's side:** compute-shader skinning into per-instance output vertex buffers. That one path
  serves raster, previous-frame positions for motion vectors, and BLAS refits for ray tracing.

---

## 3. What Rendering commits to providing

- Rendering only adds files in its own module directories, plus build entries that follow Core's conventions.
- Public renderer headers expose no Vulkan types. Backend types stay inside `rhi_vulkan`.
- A null RHI backend, so Core and CI can run the full engine headless without a GPU.
- Stable `RenderScene`, `Renderer` and asset-plugin interfaces, versioned in this doc. Breaking
  changes go through a PR with Core review.

## 4. Suggested `AGENTS.md` section for graphics

```markdown
### Graphics (owner: Claude)
- Owns: engine/rhi*, engine/render, engine/gfx_assets, shaders/, tools/shaderc, docs/rendering.
- Must not modify core modules; interface changes are proposed via issue + PR to Core.
- Public render headers must not include Vulkan or other backend headers.
- Every RHI/render change ships with tests runnable on the null backend; Vulkan tests must pass
  under validation layers with zero errors.
- Shaders are HLSL compiled offline with saige-shaderc; no runtime GLSL.
```
