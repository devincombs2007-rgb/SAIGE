# Image-based lighting data: HDR environments, SH9 diffuse and a prefiltered specular mip chain

Milestone M5. `saige/gfx_assets/hdr_image.hpp` decodes a Radiance `.hdr` file; `saige/gfx_assets/environment.hpp` turns an equirectangular HDR image into
the data a PBR shader needs (the CPU slice, #82); the GPU slice below binds the result to the model shader behind an explicit opt-in
(the shipped ambient is not the irradiance of the gradient it models, so adopting IBL changes the look; see `m5-m6-research.md` in #61, section 2.2).

## What is produced

| Part | Representation | Why |
| --- | --- | --- |
| Diffuse | nine SH coefficients per channel (27 floats), `sh_diffuse(sh, n)` is the cosine-convolved radiance factor (irradiance / pi) | exact for smooth skies, evaluated in the shader as nine multiply-adds per channel, no texture and no new RHI feature |
| Specular | **one equirectangular 2D texture with a mip chain**: mip `i` is the environment GGX-prefiltered at perceptual roughness `i / (levels - 1)`, mip 0 the unfiltered mirror | the shader samples with `lod = roughness * (levels - 1)`, so trilinear filtering interpolates between roughness levels; no cube texture, per-mip render target or compute support is needed |
| BRDF | the shipped 64x64 split-sum table | unchanged |

Mapping (engine space, +Z up, +X forward, +Y left): `u = atan2(d.y, d.x) / (2 pi) + 0.5`, `v = acos(d.z) / pi`, `v = 0` at the top, `u = 0.5` toward +X.
`Environment::sample(direction, roughness)` is the CPU reference of the shader's lookup. `to_rgba16f()` produces the upload data (RGBA16F, every level, largest
first, saturating at 65504).

## How it is computed

- **SH9** by quadrature over the source pixels with solid-angle weights. `diffuse_radiance_limit > 0` clamps each source pixel's largest channel before
  the projection: SH9 is unreliable with a bright sun disc in the environment (the error concentrates facing away from the sun), and the sun is a directional
  light, so the diffuse term should not carry it. The specular levels are *not* clamped: reflections keep the sun at full strength.
- **Prefilter**: split-sum (N = V = R), GGX importance sampling with Hammersley points, filtered importance sampling (each sample reads a source pyramid
  level chosen from the sample's solid angle, which removes the speckle of plain sampling), `n.l`-weighted mean. Deterministic: the same input gives the same bits
  in the same build. Cost is `samples` lookups per texel of every level above 0 (the default 256x128 base, 5 levels, 128 samples takes well under a second).
- **Resolution**: an equirect map resolves the poles poorly below 16 texels of width (the nearest texel centers are 45 degrees from the pole), so keep the last
  level at least 16 wide: base width 256 allows 5 levels, 512 allows 6. Roughness 1 is nearly a diffuse lobe and tolerates coarseness elsewhere.

## The HDR decoder

Bounded and hostile-input safe like the PNG/JPEG decoder: encoded size, header length, dimensions and decoded size are checked against `HdrLimits` before any
allocation; every read is bounds-checked; errors are returned. Supported: `#?RADIANCE`/`#?RGBE`, `FORMAT=32-bit_rle_rgbe`, `-Y h +X w` and the flipped
`+Y h +X w`, flat scanlines and the "new" run-length encoding. Refused with a clear error: XYZE, other orientations, the old run-length encoding. Written without
a new dependency (no stb HDR path), so there is no new license to review.

## Verification

`tests/test_environment.cpp`: a uniform environment returns its radiance everywhere (diffuse, every roughness); a linear sky's diffuse term matches the closed
form `a + (2/3) b n.z` and an independent quadrature; with a sun lobe SH9 is demonstrably worse and the radiance limit removes it from the diffuse term only;
the prefiltered levels match an independent 20000-sample GGX ground truth at 15 direction/roughness pairs; roughness blurs the sun monotonically; the
precompute is bit-identical twice; invalid settings and images are rejected; half-float conversion is exact on every representable half (every 7th checked),
rounds ties to even and saturates. `tests/test_hdr_image.cpp`: flat and run-length decoding agree, the flipped orientation, ten kinds of hostile input
including truncation at every length. Eight mutants (SH band weights, `n.l` weighting, the radiance limit, solid-angle weighting, `alpha = roughness^2`, ties to
even, the run width check, the orientation flip) each fail a test. No physical-GPU claim: this slice is CPU only.

## Using it on the GPU

```cpp
auto result = gfx::precompute_environment(hdr, settings);                       // CPU, once, at load time
RendererSettings rs;
rs.model_shading = ModelShading::MetallicRoughness;                             // required: the other modes would ignore the environment
rs.environment = std::make_shared<const render::EnvironmentLighting>(gfx::to_environment_lighting(*result.environment, /*intensity=*/1.0f));
SceneRenderer renderer(device, format, rs);
```

* **The source is explicit**: `RendererSettings::environment` (forwarded to `ModelRenderSettings::environment`) is the only way in. Null (the default) is exactly
  the previous renderer: the shader permutation without IBL is selected, nothing is allocated, and its SPIR-V is byte-identical to before. Setting an
  environment with a shading mode other than `MetallicRoughness`, or invalid data (extent, level count, texel count, non-finite coefficients, negative or
  non-finite intensity), throws `std::invalid_argument` before anything is acquired from the device.
* **Why a render-level type**: the render module must not depend on the importer, so `render::EnvironmentLighting` is plain data (nine `Float3`, the
  RGBA16F texels of every level, the level count, an intensity) and `gfx::to_environment_lighting` converts a precomputed `Environment`.
* **Shaders**: `SAIGE_IBL` is a permutation of `model.hlsl` (wrapper files `model_ibl.hlsl`, `model_ibl_shadowed.hlsl`; four permutations with the sun
  shadow one). In the metallic-roughness path the hemispherical ambient is replaced by `ibl_diffuse(n)` (the SH9 evaluated with the clamped-cosine band weights
  1, 2/3, 1/4, the same arithmetic as `sh_diffuse`) for the diffuse term and `ibl_specular(r, roughness)` (one `SampleLevel` of the equirect texture at
  `lod = roughness * (levels - 1)`) for the specular term; the split-sum scale and bias, occlusion and the sun are unchanged. Set 1 gains binding 6 (texture),
  7 (sampler: repeat in u, clamp in v, maximum LOD `levels - 1`) and 8 (a 160-byte uniform block: the nine coefficients and `levels - 1`, intensity).
* **Resources**: one RGBA16F texture with `levels` mips, one sampler, one uniform block, created together and rolled back together (the constructor
  acquires through the same `acquire()/release()` path as the shadow resources). They are static for the renderer's lifetime: runtime environment changes are
  not part of this slice.
* **Intensity** scales the sky's diffuse and specular contribution only; it is not an exposure and the sun is independent of it.

Conventions (the contract Core asked to have pinned):
* **Directions**: world space, z up. Texel (u, v) of the equirect image is the direction `(sin(pi v) cos(phi), sin(pi v) sin(phi), cos(pi v))` with
  `phi = (u - 1/2) 2 pi`; equivalently `u = atan2(y, x) / 2 pi + 1/2` and `v = acos(z) / pi`. +X is the middle of the image, +Z the top row, +Y a quarter turn
  to the right of +X.
* **Units**: linear scene radiance, the unit of the sun color and `ambient_ground`. A uniform environment of radiance L lights a white diffuse
  surface to L, exactly like a uniform ambient of the same value (tested). No exposure is applied in the shader; the tone map owns exposure. `intensity`
  multiplies the environment only.
* **Seam and poles**: the sampler repeats in u, so bilinear filtering crosses the longitude seam (u = 0 and u = 1 are the same direction), and clamps in v,
  so the poles read the top and bottom rows without wrapping to the opposite pole.
* **Roughness to LOD**: `lod = roughness * (levels - 1)`, trilinear. Level i holds the environment prefiltered at roughness `i / (levels - 1)`; roughness is the
  shading roughness clamped to the BRDF's minimum. A one-level map (`levels == 1`) is valid: the maximum LOD is 0 and every roughness reads level 0.
* **SH9**: coefficients of the radiance in the real basis (order 00, 1-1, 10, 11, 2-2, 2-1, 20, 21, 22, z up, as `sh_basis` in `environment.cpp`). The lookup
  weights band l by the clamped cosine's 1, 2/3, 1/4 and returns irradiance divided by pi, the factor the Lambert term multiplies.
* **Capability**: the environment texture is RGBA16F sampled with linear filtering across mips. `Device::supports_filtered_sampling(Format)` answers from
  `VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | SAMPLED_IMAGE_FILTER_LINEAR_BIT` (required of every conformant Vulkan device for this format); the model renderer asks
  before creating anything and throws `std::invalid_argument` when the answer is no. A renderer without an environment never asks.
* **With shadows**: IBL and sun shadows use distinct bindings in set 1 (3..5 for shadows, 6..8 for IBL) and the combined permutation; the sun is the only
  term the shadow removes.

Backend assumptions: no new device capability beyond the query above. Sampling uses an explicit LOD, so it is valid in any control flow; the texture is sampled as filterable
RGBA16F, which every Vulkan 1.3 device supports for sampled images. Equirect sampling has the usual seam at u = 0/1 (the sampler repeats) and pole pinching
(the reason the last level stays at least 16 wide).

## GPU verification

`engine/rhi_vulkan/tests/test_ibl.cpp` (validated Vulkan, lavapipe in CI):
* a uniform environment renders within 3 8-bit levels of the previous uniform-ambient path for dielectric and metal at three roughnesses (the existing path is
  the oracle);
* a sky varying with z differs from the hemispherical ambient by exactly the documented 2/3 diffuse factor, looking down and up;
* the diffuse term follows linear environments along x, y and z from four viewpoints, and pure band-2 harmonics are weighted by 1/4 (about 300 probes);
* a white metal mirror equals `k L(r)` per channel at about 190 probes, which pins the equirect mapping and the roughness-to-mip rule; rougher metal reflects
  a progressively blurrier high-frequency sky; intensity scales diffuse and specular;
* filtering crosses the longitude seam (a sawtooth in longitude gives intermediate pixels there, for a one-level and a two-level 16-texel map) and the poles
  read their own row;
* with sun shadows on, a pixel in an occluder's shadow equals the environment-only render and a pixel outside the shadow equals the unshadowed render;
* the device capability query and the refusal path (the hook simulates a device without linear RGBA16F filtering), nothing without an environment changes,
  and refused configurations throw.

`engine/render/tests/test_scene_renderer.cpp` (null device): off allocates nothing for the environment, on allocates its resources, with and without shadows,
and releases them; a creation-failure sweep over every acquisition step leaves the device at its baseline and the renderer usable; every invalid
environment is refused before acquisition; the capability refusal acquires nothing.

These are software-Vulkan correctness results. They say nothing about physical-GPU cost; the 60 FPS verdict remains `not_evaluated`.

## Not here

Cube textures, runtime environment changes, an environment owned by the scene rather than the renderer settings (Core has not yet decided that ownership;
this slice keeps the source explicit in the settings), parallax correction, bent normals, the energy-compensation term and sky rendering from the environment
(the background is still the flat sky color).
Environment assets must be original or have documented provenance and a compatible license: tests use procedural skies, none is downloaded.
