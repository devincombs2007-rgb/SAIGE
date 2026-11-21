# Rendering Test Strategy

**Status:** Proposal. The framework choice depends on D-17.

## 1. Test tiers

| Tier | Needs GPU? | Runs in CI | Examples |
|------|-----------|------------|----------|
| **Unit** | No | Every PR | Handle pools, generation checks, format utilities, render graph compile (pass culling, topological order, barrier derivation, aliasing), permutation expansion, material param packing |
| **Null-backend integration** | No | Every PR | Full renderer frame on `rhi_null`. Asserts recorded commands, barrier sequences and resource lifetimes. Asset import → cook → load round trips. |
| **Shader compilation** | No | Every PR | Every shader and permutation compiles with DXC and passes `spirv-val`, and the reflection checks pass |
| **Vulkan smoke** | Software OK | Every PR (Linux, lavapipe) | Device creation, offscreen clear, triangle, compute dispatch, buffer upload/readback. **Zero validation errors** allowed. |
| **Golden image** | Software OK | Nightly → PR once stable | Fixed scenes rendered offscreen, compared to reference images with a perceptual metric (FLIP) and per-test tolerance |
| **Hardware GPU** | Real GPU | Nightly / manual (self-hosted runner) | Ray tracing, mesh shaders, performance benchmarks with regression thresholds |

## 2. Principles

- Every RHI or renderer change ships with tests that run on the **null backend**, so contributors and
  CI never need a GPU for the core suite.
- Vulkan tests install a debug-utils messenger that **fails the test on any validation error or warning**.
- Tests that need unavailable features (no Vulkan ICD, no ray tracing support) **skip with a reason**.
  They never fail silently.
- Deterministic rendering in golden tests: fixed camera, fixed time, TAA jitter off or seeded, no
  async timing dependencies.
- Golden references are stored with Git LFS (or a separate assets repo) and regenerated only through
  an explicit, reviewed script.

## 3. CI layout (proposed, coordinated with Core's CI)

```
jobs:
  build-test-linux:    gcc + clang, unit + null-backend + shader compile + Vulkan smoke (Mesa lavapipe)
  build-test-windows:  msvc, unit + null-backend + shader compile (Vulkan smoke if a runner GPU/WARP-equivalent is available)
  golden-nightly:      lavapipe offscreen golden images, uploads diff artifacts on failure
```

## 4. Phase 1 test checklist (initial tests)

- [ ] `rhi::Handle` pool: allocate, free, reuse and stale-handle detection
- [ ] Bindless index allocator: stable indices, free-list reuse, exhaustion error
- [ ] Format table: block sizes, sRGB pairs, depth/stencil flags
- [ ] Null device: create/destroy all resource types, deferred destruction respects frame timeline
- [ ] Vulkan: instance and device creation under validation (skips if no ICD)
- [ ] Vulkan: offscreen clear to known color → readback → exact match
- [ ] Vulkan: triangle render → readback → pixel coverage check
- [ ] `saige-shaderc`: annotation parsing, permutation expansion and exclusion, cache-key stability
