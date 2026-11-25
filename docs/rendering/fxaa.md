# FXAA

An opt-in post-process anti-aliasing pass: it smooths the aliased edges of the finished image in one extra full-screen draw. Off by default; with it off nothing is created and every image is what it was
before.

```cpp
RendererSettings rs;
rs.fxaa.enabled = true;            // off by default; works with hdr on or off
rs.fxaa.edge_threshold = 0.125f;   // luma range relative to the brightest neighbour below which a pixel is left alone (0 to 1; lower = more edges)
rs.fxaa.edge_threshold_min = 0.0312f;  // absolute luma range below which a pixel is left alone (0 to 1; keeps dark noise out)
rs.fxaa.span_max = 8.0f;           // the longest blend along an edge, in texels (1 to 32)
```

Invalid values throw `std::invalid_argument` before anything is created; disabled settings are not validated. `FrameStats::fxaa_draw_calls` is 1 while it runs.

## The algorithm (shaders/scene/fxaa.hlsl; CPU reference `fxaa_apply`)

The compact form of Timothy Lottes' FXAA, on the finished (display-referred) image:

1. Fetch the pixel and its four diagonal neighbours and compute their **perceptual luma**: the color raised to `luma_exponent` (0.5 when the sampled texture is an sRGB format whose decode the
   sampler undid, so the contrast is judged in the encoded space the eye sees; 1 for a texture that already holds display values), weighted 0.299, 0.587, 0.114.
2. If the luma range of those five is below `max(edge_threshold_min, brightest * edge_threshold)`, return the pixel **unchanged** (the early-out: flat regions, noise and gentle gradients are
   bit-exact copies).
3. Otherwise the luma gradient gives the edge direction (perpendicular to it, scaled so its shorter component is about one texel, clamped to `span_max`), and the pixel is a blend of bilinear taps
   along that direction: `A` = the two taps at +-1/6 of the span, `B` = A and the two taps at +-1/2. `B` is used unless its luma leaves the neighbourhood's range (then `A`).

An axis-aligned straight edge is left alone (the blend runs along it through equal pixels); diagonal and curved edges get the intermediate values that anti-aliasing would have produced.

## Where it runs

With FXAA on, the scene (and the tone map, in HDR) renders into an intermediate target of the **output's format and size** (`FxaaInput`) instead of the output, and the FXAA pass reads it and writes the
output. The intermediate target is created with the other size-dependent targets (all-or-nothing on resize, like the HDR target) and costs one color target of the output.

## Limits

* FXAA works on the final image: it cannot recover detail that was never sampled (a wire thinner than a pixel that skips pixels, shader aliasing inside a texture), it slightly softens fine texture and text, and
  it has no temporal component (it shimmers a little in motion). It is a cheap, content-independent option, not MSAA or TAA.
* It runs after tone mapping, on display values; there is no variant that runs on HDR color (a bright edge that tone mapping compresses is judged after compression, which is what the eye sees).
* Software Vulkan evidence only; no physical-GPU timing is claimed.

## Verification

* `engine/render/tests/test_fxaa.cpp` (CPU): ranges, luma, flat regions / axis-aligned edges / low-contrast noise copied exactly, a diagonal staircase smoothed in a narrow band with nothing changed farther
  than 3 px from the edge and a convex result, the thresholds and span as real parameters, the exponent changing what counts as an edge.
* `engine/render/tests/test_scene_renderer.cpp` (null device): refusals, no pass or resource when off, one extra draw / target / group on (LDR and HDR), resize without a leak, failure injection.
* `engine/rhi_vulkan/tests/test_fxaa.cpp` (validated Vulkan with synchronization validation, lavapipe in CI): the pass against the CPU reference texel by texel on a synthetic image full of aliased edges, for a UNORM and an
  sRGB target and for default and tuned settings (worst difference 1 level, 4 on the one tuned UNORM run where a few pixels sit on the narrow-or-wide decision); through the scene renderer in LDR and HDR: every pixel whose
  5 x 5 neighbourhood in the unsmoothed image is flat is unchanged, the cube silhouettes are smoothed within a narrow band, the result is deterministic.
* Fifteen mutants (the direction components, the thresholds, the reduction, the final choice, the luma weights and exponent, the span, the taps, the pass's exponent, texel size and validation, the graph wiring and the
  counter) each fail a test; the graph-wiring mutants are caught by the synchronization validation.
