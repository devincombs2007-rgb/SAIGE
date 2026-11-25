# Bloom

An opt-in HDR post effect: a thresholded, blurred copy of the scene is added back before tone mapping, so very bright things glow into their surroundings. Off by default;
with it off nothing is created and every image is what it was before (the existing HDR images are unchanged; the plain tone-map permutation's SPIR-V differs only in its
source-hash lines).

## Using it

```cpp
RendererSettings rs;
rs.hdr = true;                    // required: bloom of an LDR image is meaningless (nothing is brighter than 1)
rs.bloom.enabled = true;          // off by default
rs.bloom.intensity = 0.05f;       // how much glow is added (0 to 16)
rs.bloom.threshold = 1.0f;        // scene-referred brightness (largest channel) where glow begins
rs.bloom.knee = 0.5f;             // soft-knee width, as a fraction of the threshold (0 = hard)
rs.bloom.scatter = 0.7f;          // how much of each wider level is added back (0 to 1)
rs.bloom.levels = 5;              // 1 to 8; reduced automatically for tiny targets
SceneRenderer renderer(device, format, rs);
```

`RendererSettings::bloom.enabled` without `hdr`, or with an invalid value, throws `std::invalid_argument` before anything is created. Disabled settings are not validated.
`FrameStats::bloom_draw_calls` is `2 * levels - 1` while it is on.

## The chain

With `D` for downsample levels and `U` for the levels carried back up:

```
D_0 = downsample13(prefilter(scene))        half the scene size; the threshold is applied to every tap
D_i = downsample13(D_{i-1})                 half the size again, never below 1 x 1
U_{n-1} = D_{n-1};   U_i = D_i + scatter * tent3x3(U_{i+1})
output = tone_map(scene + intensity * bilinear(U_0))      exposure and the operator come after, as for every pixel
```

* **13-tap downsample** (Jimenez, *Next Generation Post Processing in Call of Duty: Advanced Warfare*): twelve bilinear taps around the destination pixel and the center,
  center 0.125, the four corners at (+-2, +-2) 0.03125, the four edges at (0, +-2) and (+-2, 0) 0.0625, the four inner diagonals at (+-1, +-1) 0.125 (they sum to 1; offsets in
  source texels). A wide, stable low-pass without the shimmer of a box filter.
* **Soft-knee threshold**, per tap, with `b` the largest channel, `knee_width = threshold * knee`: `q = clamp(b - threshold + knee_width, 0, 2 knee_width)^2 / (4 knee_width +
  1e-5)`, weight `max(q, b - threshold) / b`. Nothing below `threshold - knee_width`, a straight line above `threshold + knee_width`, a quadratic between; the hue is kept.
* **Tent upsample**: nine bilinear taps one coarse texel apart, weights `[1 2 1; 2 4 2; 1 2 1] / 16`.
* **No blending**: blending into `RGBA16_FLOAT` is not part of the RHI's portable subset, so every `U_i` is its own texture and reads both `D_i` and `U_{i+1}`; the RHI has no
  compute, so every step is a full-screen render pass.
* **Sanitation**: every tap is cleaned first (NaN and negatives become 0, values above 65504 are clamped), so one bad pixel cannot spread through the chain. Bilinear
  filtering of a NaN or an infinity is implementation-defined (a zero weight times an infinity is NaN on some hardware), so such texels are *bounded* (the glow stays finite and
  not negative) but not reproducible texel for texel; an HDR scene should not contain them in the first place.

The CPU functions in `render/bloom.hpp` (`bloom_downsample`, `bloom_combine`, `bloom_chain`, `bloom_composite`) are the reference of every step, with the hardware's texel-center
convention (a texel's center is at `(i + 0.5) / size`, clamp to edge).

## What it costs

* Memory: the chain is half the scene size and a geometric series below it, for both `D` and `U`: about two thirds of one HDR target's memory in `RGBA16_FLOAT`
  (for example 1920 x 1080: 8 bytes x (960 x 540) x (1 + 1/4 + 1/16 + ...) x 2 minus the unused top `U` is roughly 11 MiB).
* Passes: `2 * levels - 1` draws of a full-screen triangle on shrinking targets, plus an extra texture fetch pair in the tone-map pass. The first level is the most expensive (it
  reads the full-size scene with 13 taps).
* The chain is rebuilt when the target size changes, with all-or-nothing creation (a failed rebuild leaves the previous chain and the frame fails, not the renderer).
* No physical-GPU timing is claimed; software Vulkan only.

## Verification

* `engine/render/tests/test_bloom.cpp` (CPU): settings ranges, level counts and sizes, bilinear sampling, uniform images stay uniform at any size, a hand-computed impulse footprint
  (0.0625), cleaning, the threshold curve (below the knee, the straight line, monotone, continuous, hue kept), the tent's total weight, the geometric sum of a uniform bright scene,
  hostile input and falloff with distance.
* `engine/render/tests/test_scene_renderer.cpp` (null device): refused without HDR or with bad values and nothing created, no pass and no resource when off, `2 * levels - 1`
  draws and a released chain when on, resize without a leak, tiny targets, failure injection at every creation of a rebuild.
* `engine/rhi_vulkan/tests/test_bloom.cpp` (validated Vulkan, lavapipe in CI): the whole GPU chain against the CPU reference texel by texel for seven sizes and settings (worst error
  a quarter of the tolerance), non-finite input bounded, the tone-map bloom permutation against `tone_map(scene + intensity * bilinear(bloom))` for every operator, three
  intensities and two exposures, and through the scene renderer: a bright emissive cube glows (every pixel just outside it brighter, far pixels a small fraction as much), intensity 0
  is byte-identical to no bloom, frames are deterministic, resizing and tiny targets work. The first run of the chain test found a real bug (the up-pass bind groups read the
  chain's level count before it was set), which is why the test compares every level.

## Limits and next

* HDR only; no lens dirt, no star or anamorphic streaks, no temporal stabilisation (a very bright single pixel can still flicker at the first level; a Karis average on the first
  downsample is the usual fix and is not done).
* Auto-exposure is not part of this change; the tone-map exposure stays manual (`ToneMapSettings::exposure_ev`). Bloom is added before exposure, so a manual exposure change scales
  the glow with the scene.
* The hardware's bilinear weights are 8-bit fixed point on some GPUs: a small, smooth difference from the float reference, not tested on physical hardware.

## Mutation check

Twenty-six mutants of the new code (filter weights, the threshold curve and its knee, the tent, cleaning, level counts, clamps, the up and down passes' sizes, sources and push
constants, the pipeline choice, the graph's wiring, the HDR requirement, the composite's intensity and its sanitation, the tone mapper's bloom guard) were each run against the
CPU, null-device and validated Vulkan suites: all killed or not buildable (one that only drops the scatter factor on the CPU does not compile under warnings-as-errors; the GPU
form of it is killed). Three survivors led to changes: the per-texture state tracking between frames was redundant (every level is fully overwritten each frame, so each starts
undefined) and was removed, and the tone mapper's bloom guard and intensity clamp got tests.
