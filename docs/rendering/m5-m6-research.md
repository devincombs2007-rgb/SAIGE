# M5 image-based lighting and M6 cascaded sun shadows: research and preparation

Status: research and CPU-only preparation. **No GPU implementation, RHI change or shader change is proposed to start without Codex's
architectural approval.** What exists on this branch: `docs/rendering/m5-m6-research.md` (this file), CPU references for M5
(`engine/gfx_test_support/include/saige/gfx_test/ibl_reference.hpp`, tests in `tests/test_ibl_reference.cpp`) and CPU cascade fitting
for M6 (`engine/render/include/saige/render/shadow_cascades.hpp`, tests in `tests/test_shadow_cascades.cpp`). Both are pure math with no
GPU objects and no dependency on unreviewed RHI contracts. Context: [material-architecture.md](material-architecture.md) sections 6 and 7.

## 1. What the RHI offers today (checked in code, not assumed)

| Need | State on main |
|---|---|
| 2D textures, mips, array layers (upload covers every mip and layer) | yes; image views are `2D` or `2D_ARRAY` only |
| Cube textures | `TextureDimension::TextureCube` exists in the description, but the Vulkan backend creates no cube-compatible image and no cube view |
| Render into one mip or one layer of a texture | **no**: attachments name a whole texture; no per-mip or per-layer view |
| Comparison (shadow) sampler | `SamplerDesc::compare_enable` / `compare_op` exist |
| Depth-only pipelines | allowed in validation (a depth format and no color formats); untested on Vulkan |
| Depth bias | **not implemented as dynamic state** (only viewport and scissor are dynamic), although `rhi-design.md` says depth bias is dynamic |
| Compute, storage images | none yet |
| Blend state | #57 (open) |
| Push constants | 128 bytes, fully used by the model pipeline (`ModelPush`); new per-frame data goes in uniform buffers (set 1) |
| Binding limits | 16 sampled textures, 16 samplers, 12 uniform buffers per pipeline; the model pipeline uses 6, 6 and 2 |

## 2. M5: image-based lighting

### 2.1 Goal
Replace the hemispherical ambient (`ambient_radiance` in `model.hlsl`) with lighting from an environment: diffuse irradiance and
prefiltered specular, reusing the split-sum BRDF table (#55). The sun stays a directional light.

### 2.2 Findings from the CPU references (measured)
- **The shipped ambient is not the irradiance of the gradient it models.** `pbr.hlsli` lights the diffuse term with L(n). For a sky
  that varies linearly with z, L = a + b d.z, the exact diffuse radiance factor is a + (2/3) b n.z (closed form, verified by SH and by
  quadrature to 2e-3). The shipped ambient therefore exaggerates the sky-to-ground contrast by 3/2; with the renderer's colors the
  largest difference is 0.055 in the blue channel, facing straight up or down. Moving to IBL changes the look of every surface by about
  that much. It is a correctness improvement, but it is a visible change, so it should arrive with the explicit shading-mode opt-in the
  owner already requires, not silently.
- **SH9 (9 coefficients) is exact for a smooth sky and unreliable with a bright sun lobe in the environment.** Worst relative error of
  the diffuse factor over six test normals: 0.0000 for the gradient, 0.174 for the same sky plus an 8x sun lobe (error concentrates facing
  away from the sun, where the true irradiance is small). Design consequence: **exclude the sun from the SH/irradiance environment** (it
  is a directional light) or use an irradiance cube; do not feed a raw HDR sky that contains the sun disc into SH9.
- The GGX prefilter reference (importance sampling, Hammersley points, split-sum N = V = R) reproduces a uniform environment exactly,
  returns the environment at near-zero roughness, and blurs a sun spike monotonically (the value toward the sun falls and the contrast
  against a direction 23 degrees away shrinks as roughness grows).

### 2.3 Representation options

| Part | Option A (recommended start) | Option B |
|---|---|---|
| Diffuse | SH9 of the environment **without the sun**, 27 floats in a uniform buffer, evaluated in the pixel shader (nine multiply-adds per channel); no texture, no new RHI feature | small irradiance cube (e.g. 32x32 per face) |
| Specular | prefiltered radiance cube with mips: roughness r maps to mip r * (mips - 1); 256x256 per face, 9 mips, RGBA16F is 4.2 MB (RG11B10 about half) | octahedral 2D map (no cube feature, more shader math, seam handling) |
| BRDF | the shipped 64x64 table | same |
| Energy | the approved optional energy-conserving model (Kulla-Conty style coupling) uses the same table: `E = f0 * scale + bias` | |

### 2.4 Where to precompute (this decides which RHI work M5 needs)
- **CPU at import, as a versioned asset, then upload as an ordinary texture.** Deterministic, testable against the references here,
  needs no compute and no render-to-cube RHI work, and follows the precedent of the BRDF table (#55: embedded asset, generator, verifier,
  versioned). Cost: seconds per environment on the CPU at import (acceptable for assets, not for runtime-changing environments). Needs
  cube-compatible images and cube views (RHI gap) or an octahedral 2D layout (no gap).
- GPU at load: needs per-mip, per-layer render targets (gap) or compute with storage images (compute is a larger new contract).
  Recommended later, only if runtime environment changes (time of day) require it.

### 2.5 Environment input
`.hdr` (Radiance) and `.exr` need a decoder. stb_image can read `.hdr`, but SAIGE builds it with `STBI_ONLY_PNG`/`STBI_ONLY_JPEG` and a
dimension cap; enabling HDR is a dependency-review item, not a flag flip. Alternatives: KTX2 with a prefiltered cube already baked by
an external tool (no decoder beyond KTX2 and Basis, a larger dependency), or a procedural analytic sky for tests and the first milestone
(no dependency, ideal for CI). Recommendation: procedural sky first, `.hdr` via a reviewed stb build second.

### 2.6 Shader and pipeline consequences
Set 1 gains the prefiltered cube, its sampler, and a uniform block with the SH coefficients and mip count (a texture, a sampler and a
uniform buffer on top of the six, six and two already used; well inside the 16, 16 and 12 limits). `ModelShading` gains a variant;
Legacy and the current MetallicRoughness stay as they are. Specular occlusion and horizon handling are small additions to `shade_ambient`.

## 3. M6: cascaded sun shadows

### 3.1 CPU preparation (done here, verified)
`fit_shadow_cascades` splits [near, shadow_distance] with the practical scheme (blend of uniform and logarithmic), fits an orthographic
light projection to the **bounding sphere** of each slice (size independent of camera orientation), and snaps the sphere center to whole
texels in a light basis that depends only on the sun direction. Verified by tests: splits monotone and exact at the ends; the sphere
passes through the farthest corners; every point of every slice maps inside its cascade's [-1, 1] x [-1, 1] x [0, 1] volume for three sun
directions including straight overhead; a world point keeps its fractional texel position (within 0.01 texel) across 40 arbitrary camera
moves and turns for every cascade (without snapping it moves by more than 0.05 texel in one small step); radius and texel size do not
depend on where the camera looks; cascade selection by view-space depth (the same measure the blend sort uses). Six mutations (no
snapping, wrong snap size, inverted lambda, wrong radius, short depth range, camera-dependent light basis) each fail the suite.

### 3.2 Algorithm choices
- 3 or 4 cascades, lambda about 0.75, 2048x2048 D32_SFLOAT each: 64 MB for four (32 MB with D16). Shadow distance 100 to 200 m.
- Depth-only pass per cascade into one layer of a depth 2D array; **one pass per layer through a per-layer view** (portable) rather than
  layered rendering, which needs `shaderOutputLayer` (an optional feature) or multiview (optional).
- Filtering: PCF with a comparison sampler and several taps. Linear filtering of depth formats is not guaranteed on every
  implementation (check format properties); taps with nearest comparison are portable. Cascade blending over a band at the far end of
  each cascade removes the visible seam.
- Bias: slope-scaled depth bias in the depth pass (needs pipeline depth-bias state) plus a normal-offset in the receiver, scaled by the
  cascade's `texel_world_size` (provided). `depthBiasClamp` and `depthClamp` are optional features; the light near plane is pulled
  back by `caster_margin` instead of relying on depth clamp.
- Casters: opaque and alpha-masked primitives (the mask needs an alpha test in the depth pass); blended primitives do not cast by
  default. Per-cascade culling uses the bounding spheres `GpuPrimitive` already carries (#59) against the cascade's light-space box,
  so four cascades do not mean four full scene submissions.
- Shading: `sun_visibility(world_position, normal, view_depth)` returns the lit fraction and multiplies only the directional term. Both
  shading modes can use it. Keeping it a single function is also the seam for ray-traced shadows later.

### 3.3 Cost model
Four cascades add up to four culled draws of the casters (depth only, no fragment work for opaque geometry) plus one array sample with
about 4 to 16 comparison taps per lit pixel. Measure on hardware before setting budgets: nothing here has run on a physical GPU.

## 4. Portable Vulkan 1.3 requirements

Mandatory in core Vulkan (universal): cube-compatible images and cube image views; image views of single mips and layers; 2D array
images with at least 256 layers; D16 and D32_SFLOAT or X8_D24 depth attachments with sampling; comparison samplers; RGBA16_SFLOAT and
B10G11R11 as sampled, blendable color attachments; dynamic rendering (core 1.3); depth-bias pipeline state and the dynamic depth-bias
state; uniform buffers of at least 16 KB.

Optional, to be gated by the capability probe or avoided (the capability report from #45 is the mechanism): `shaderOutputLayer`
(layered rendering without multiview), multiview, `depthClamp`, `depthBiasClamp`, linear filtering of depth formats, extended storage
image formats, `samplerAnisotropy` (already negotiated), `VK_KHR_ray_query`. Verify each against the pinned spec before relying on it;
this list is the checklist, not a substitute for it.

## 5. Ray tracing, path tracing and upscaler compatibility
- Keep the environment as bindable resources plus a CPU-side source (keep the equirect or SH/cube source so a ray-tracing miss shader
  and importance sampling can use the same environment); IBL in raster mode is the split-sum approximation of what a path tracer
  integrates, and the shared BRDF functions in `pbr.hlsli` and `SurfaceData` stay the single source of truth for both.
- `sun_visibility` becomes a ray query (`VK_KHR_ray_query`, optional) without changing shading code.
- Cascade fitting and the texel snapping remove shadow shimmer, which temporal upscalers amplify; motion vectors and history
  invalidation (Core's temporal frame data, #17) are separate prerequisites and are not touched here.

## 6. Proposed sequence and the contracts that need Codex's approval

1. M5a: procedural sky, CPU-precomputed SH9 (sun excluded) and prefiltered cube as a versioned asset; no new RHI contract if the
   specular map is octahedral 2D, or cube textures and views if a cube is chosen. Opt-in shading mode.
2. RHI: cube-compatible images and cube views; per-mip and per-layer views usable as attachments and as sampled sources; dynamic or
   static depth-bias state; a depth-only pipeline test on Vulkan. Each additive and backward compatible, like #57, with pixel tests.
3. M6a: depth-only cascade passes, `sun_visibility`, PCF, bias; acceptance tests listed below.
4. M5b/M6b: energy-conserving option, cascade blending, alpha-masked casters.

Acceptance tests to write with the GPU work (the CPU references exist): furnace test of the IBL path (white diffuse and white metal under a
uniform environment return the environment); GPU diffuse versus `diffuse_brute` within the SH tolerance; prefiltered mip versus
`ggx_prefilter` at five roughnesses; shadow acne bounded on a lit flat plane; no light leak at a contact edge; cascade seam continuity
(difference across the split below a threshold); shimmer test (the same scene rendered with sub-texel camera moves changes only at
edges that truly move).

## 7. Decisions requested
- Diffuse: SH9 without the sun (recommended) or an irradiance cube?
- Specular: cube (needs cube images and views in the RHI) or octahedral 2D (no new image kind)?
- Precompute: CPU asset first (recommended) or GPU at load (needs the larger compute or render-to-layer contracts)?
- Environment input: procedural first, then `.hdr` through a reviewed stb build?
- Blended primitives: excluded from shadow casting (recommended) or alpha-tested?
