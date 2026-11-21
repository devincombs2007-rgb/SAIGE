# Shader Compilation Strategy

**Status:** Proposal. Depends on D-02, D-03 and D-13.

## 1. Language: HLSL (Shader Model 6.6+)

| Option | Pros | Cons |
|--------|------|------|
| **HLSL + DXC** *(chosen)* | Industry standard for AAA. One source for SPIR-V (Vulkan) and DXIL (future D3D12). Mature support for ray tracing, mesh shaders, wave intrinsics and 16-bit types. Most public research code is in HLSL. | DXC's SPIR-V backend sometimes lags new Vulkan extensions. |
| GLSL + glslang | Native to Vulkan. | No DXIL path, weaker language features, little AAA sample code. |
| Slang | Modules, generics, autodiff, many targets, Khronos-hosted. | Smaller ecosystem and tooling. Adds a second compiler to support. |

**Decision:** Author HLSL and compile it with **DXC** to SPIR-V (`-spirv -fspv-target-env=vulkan1.3`).
Shaders stick to the HLSL subset that Slang also accepts, so moving to Slang later (for modules or
autodiff in neural rendering) stays cheap. This is rendering-internal and needs no Core decision.

## 2. Offline compilation with `saige-shaderc`

Shaders are **compiled offline at build/cook time**, never from source in shipping builds.

```
shaders/**/*.hlsl  ──►  saige-shaderc  ──►  build/shaders/<target>/<hash>.spv  +  shaders.manifest
                         │  ├─ expand permutations
                         │  ├─ dxc compile (+ debug info in dev)
                         │  ├─ spirv-val validation
                         │  ├─ SPIRV-Reflect: verify bindless layout & push constant size
                         │  └─ content-addressed cache
```

- **Integration:** a CMake custom command per shader *library* (not per file). The tool does its own
  dependency tracking through the `#include` graph, so editing a header rebuilds only the affected shaders.
- **Cache key:** `hash(source + resolved includes + defines + entry point + stage + target + dxc version + tool version)`.
  The cache is content-addressed, so identical permutations are deduplicated.
- **Manifest:** maps `(shader name, permutation key) → blob`, includes reflection metadata, and is
  loaded at runtime by the RHI pipeline factory.
- **Targets:** `spirv-vk13` now, `dxil-sm66` reserved for a future D3D12 backend.

## 3. Shader declarations & permutations

Each shader file declares its entry points and permutation dimensions in a header comment block.
`saige-shaderc` parses this block, so no separate build list is needed:

```hlsl
// @shader   GBufferPass
// @entry    VSMain vs
// @entry    PSMain ps
// @option   ALPHA_TEST       {0, 1}
// @option   NORMAL_MAP       {0, 1}
// @option   VERTEX_COLORS    {0, 1}
// @exclude  ALPHA_TEST=1 && VERTEX_COLORS=1      // prune invalid combos
```

- Permutation key: a 64-bit bitfield built from the option values, stable across builds.
- **Budget rule:** CI fails if a shader exceeds its declared permutation budget (default 64). Rarely
  used features go through dynamic branches on uniform values instead, since modern GPUs handle
  uniform branches well.
- The material system (Phase 2+) produces the material permutations. Bindless materials are designed
  to keep that count small: most material variation is data, not code.

## 4. Shared C++/HLSL headers

Struct layouts shared between CPU and GPU live in `shaders/shared/*.hlsli`, and C++ includes them
through a small compatibility header:

```hlsl
// shaders/shared/instance_data.hlsli
#include "shared/hlsl_cpp_compat.hlsli"   // maps float4x4, uint, etc. to C++ types when included from C++
SAIGE_GPU_STRUCT(InstanceData)
{
    float3x4 localToWorld;
    float3x4 prevLocalToWorld;
    uint     meshIndex;
    uint     materialIndex;
    uint     flags;
    uint     pad0;
};
SAIGE_STATIC_ASSERT_SIZE(InstanceData, 112);
```

- Layout uses the `scalarBlockLayout` rules, with `-fvk-use-scalar-layout` passed to DXC.
  `static_assert` on size and offsets catches divergence at compile time.
- Shared constant enums (light types, material flags) are defined once in the same headers.

## 5. Bindless conventions

`shaders/common/bindless.hlsli` declares the global descriptor arrays that match
[rhi-design.md §5](rhi-design.md#5-descriptors-bindless-first), plus typed accessors:

```hlsl
Texture2D    GetTexture2D(uint index) { return g_Textures2D[NonUniformResourceIndex(index)]; }
SamplerState GetSampler(uint index)   { return g_Samplers[index]; }
// Where DXC's SPIR-V backend supports SM6.6 ResourceDescriptorHeap well enough, the accessors switch to it.
```

Index `0` of every array is a valid "error" resource (magenta texture, zero buffer), so invalid
indices are visible on screen instead of crashing the GPU.

## 6. Development workflow

- **Hot reload (dev builds):** Core's file watcher (D-13) reports a changed `.hlsl` or `.hlsli` file.
  The engine invokes `saige-shaderc` (library or subprocess), swaps the affected pipelines at the next
  frame boundary, and keeps the old pipeline if compilation fails, logging the error.
- **Debugging:** dev builds embed source-level debug info (`-Zi -fspv-debug=vulkan-with-source`) for
  RenderDoc and Nsight shader debugging. Release builds strip it and optimize (`-O3`).
- **Warnings as errors** for shaders in CI (`-WX`).

## 7. Tests

- CI job: compile **every** shader and permutation for every target, then run `spirv-val`. Any failure blocks merge.
- Reflection tests: push constants ≤ 128 bytes, and shaders bind only the global bindless set.
- `saige-shaderc` unit tests: annotation parser, permutation expansion and exclusion, cache-key
  stability, include-dependency tracking.
