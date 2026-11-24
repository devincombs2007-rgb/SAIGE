# Image-based lighting data: HDR environments, SH9 diffuse and a prefiltered specular mip chain

Milestone M5, CPU slice. `saige/gfx_assets/hdr_image.hpp` decodes a Radiance `.hdr` file; `saige/gfx_assets/environment.hpp` turns an
equirectangular HDR image into the data a PBR shader needs. Nothing here touches the GPU or the renderer yet: the next slice binds the result
to the model shader behind an explicit opt-in (the shipped ambient is not the irradiance of the gradient it models, so adopting IBL changes
the look; see `m5-m6-research.md` in #61, section 2.2).

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

## Not here

The GPU consumption (a `SAIGE_IBL` shader permutation, set 1 bindings for the SH uniform and the environment texture, a `SceneRenderer::set_environment`,
pixel tests against the CPU reference), cube textures, runtime environment changes, parallax correction, bent normals and the energy-compensation term.
Environment assets must be original or have documented provenance and a compatible license: tests use procedural skies, none is downloaded.
