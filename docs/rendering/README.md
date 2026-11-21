# SAIGE Rendering & Asset Systems — Design Proposal

**Status:** ✅ Architecture direction **approved by the project owner** (2026-11-21), subject to integration
review with Core (ChatGPT/Codex) and testing.
**Owner:** Rendering & Asset Systems (Claude)
**Scope:** Graphics backend (RHI), Vulkan backend, renderer, shaders, materials, textures, graphics asset pipeline.

> No code in this directory depends on, or changes, core engine code. Everything here is a proposal
> that must be reconciled with the core architecture before implementation begins.

---

## 1. Repository review (2026-11-21)

| Expected artifact            | Found? | Notes                                                        |
|------------------------------|--------|--------------------------------------------------------------|
| `AGENTS.md`                  | No     | Agent roles, ownership, and conventions are not yet written down. |
| Architecture specification   | No     | No module layout, threading model, or coding conventions.    |
| Core runtime source          | No     | No build system, platform layer, ECS, job system, or math library. |
| Build system                 | No     | No CMake or other build files.                                |
| Issues / PRs                 | None   | —                                                            |
| `README.md`                  | Yes    | Two lines. Expanded SAIGE as "Spectacular…". **Resolved:** the owner standardized the name as **Sensational Artificial Intelligent Game Engine**. |

**Conclusion:** Every interface the renderer depends on is still undefined: windowing, job system,
memory, file system, math, ECS, asset registry and build conventions. As the owner instructed,
rendering will **not** implement dependent code yet. This directory documents:

1. The interfaces Rendering needs from Core, with proposed shapes ([core-interface-requirements.md](core-interface-requirements.md)).
2. The rendering architecture we intend to build on top of those interfaces.

## 1a. Owner decisions (2026-11-21)

| Decision | Outcome |
|----------|---------|
| Project name | **SAIGE = Sensational Artificial Intelligent Game Engine** (standardized project-wide) |
| D-06 Coordinate system | **Right-handed, Z-up, meters**, as the engine-wide standard: [docs/standards/coordinate-system.md](../standards/coordinate-system.md) |
| Animation ownership (D-19) | Animation is an independent engine subsystem. **Core (Codex)** owns its core interfaces and integration. **Rendering (Claude)** owns skeletal mesh rendering, GPU skinning, and graphics-related asset import (skeletons, skins, clips from glTF). Playback, blending, retargeting and IK use a separate modular architecture, with ownership to be agreed before implementation. |
| Rendering architecture | Approved as the direction, subject to integration review and testing |

## 2. Documents

| Document | Purpose |
|----------|---------|
| [core-interface-requirements.md](core-interface-requirements.md) | **Start here, Codex.** Numbered decisions (D-01…D-18) Rendering needs from Core, each with a recommendation. |
| [render-architecture.md](render-architecture.md) | Layering, frame flow, threading, render graph, render scene, and the long-term feature roadmap. |
| [rhi-design.md](rhi-design.md) | Backend-agnostic Render Hardware Interface, and the Vulkan 1.3 backend design. |
| [shader-pipeline.md](shader-pipeline.md) | Shader language, offline compilation, permutations, bindless conventions, hot reload. |
| [asset-pipeline.md](asset-pipeline.md) | glTF 2.0 import, PBR material model, texture and mesh cooking, runtime formats. |
| [testing-strategy.md](testing-strategy.md) | Unit, null-backend, headless Vulkan, shader and golden-image tests, plus CI. |

## 3. Ownership boundaries (proposed)

| Area | Owner | Rendering's relationship |
|------|-------|--------------------------|
| Build system, toolchain, dependency management | Core | Consumer. Rendering adds its own targets under Core's conventions. |
| Platform (windows, input, native handles) | Core | Consumer. Needs a native window handle plus resize events. |
| Job system, memory, logging, asserts, profiling | Core | Consumer. |
| Math library | Core | Consumer. Needs GPU-compatible POD layouts. |
| ECS / scene / entities | Core | Core's ECS systems call into Rendering's `RenderScene` API. |
| Generic asset registry, IDs, VFS, streaming I/O | Core | Rendering registers graphics importers, cookers and loaders. |
| RHI, Vulkan backend, render graph, renderer | **Rendering** | — |
| Shaders, shader compiler tool, materials | **Rendering** | — |
| Mesh, texture and material import and cooking | **Rendering** | Plugs into Core's asset framework. |

Suggested `AGENTS.md` text for the graphics section is in
[core-interface-requirements.md §4](core-interface-requirements.md#4-suggested-agentsmd-section-for-graphics).

## 4. Proposed module layout (pending D-04)

```
engine/
  rhi/            saige::rhi         Backend-agnostic GPU abstraction (headers + common code)
  rhi_vulkan/     saige::rhi_vulkan  Vulkan 1.3 backend
  rhi_null/       saige::rhi_null    No-op backend for unit tests / headless CI
  render/         saige::render      Render graph, render scene, passes, material runtime
  gfx_assets/     saige::gfx_assets  Mesh/texture/material importers, cookers, runtime loaders
shaders/                             HLSL sources (+ shared C++/HLSL headers)
tools/
  shaderc/        saige-shaderc      Offline shader compiler / permutation builder
tests/
  rhi/  render/  gfx_assets/
```

All paths and names will be adapted to Core's layout once D-04 is decided.

## 5. Milestones

| Phase | Deliverable | Blocked on |
|-------|-------------|------------|
| **0** | Design docs, interface requests, issues (this PR) | — |
| **1** | RHI interface, null backend, Vulkan bring-up (device, swapchain, clear, triangle), `saige-shaderc`, unit tests. **Backend-agnostic RHI foundation started (no Core dependency).** | D-01…D-05, D-11, D-12, D-17 |
| **2** | Render graph, forward PBR, glTF static meshes, KTX2/BCn textures, IBL, cascaded shadows | D-07, D-09, D-13, D-14, D-15 (D-06 ✅) |
| **3** | GPU-driven rendering: bindless, GPU culling, indirect draws, visibility buffer, clustered lights, TAA, HDR pipeline | Phase 2 |
| **4** | Physically based sky, volumetric clouds and fog, terrain and vegetation rendering, texture streaming | Phase 3 |
| **5** | Hardware ray tracing: RT shadows, reflections and GI, denoisers, upscaler integration | Phase 3 |
| **6** | Virtualized geometry (cluster LOD DAG, streaming) and virtual shadow maps | Phases 3 and 5 |

See [render-architecture.md §7](render-architecture.md#7-feature-roadmap-toward-aaa-targets) for details.
