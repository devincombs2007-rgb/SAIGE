# Ambient occlusion

An opt-in, depth-only screen-space ambient occlusion: two extra full-screen passes that darken creases and contact points in the HDR scene color before the tone map. Off by default; with it off nothing is
created and every image is what it was before.

```cpp
RendererSettings rs;
rs.hdr = true;                              // required: AO multiplies the linear HDR color
rs.ambient_occlusion.enabled = true;        // off by default
rs.ambient_occlusion.radius = 0.75f;        // meters, 0.01 to 16: how far geometry can occlude a surface
rs.ambient_occlusion.intensity = 1.0f;      // 0 to 4; 0 is no occlusion
rs.ambient_occlusion.bias = 0.05f;          // 0 to 0.5, a fraction of the radius: how far above the surface plane a tap must be to count
```

Invalid values, and `enabled` without `hdr`, throw `std::invalid_argument` before anything is created; disabled settings are not validated. `FrameStats::ao_draw_calls` is 2 while it runs.

## The algorithm (shaders/scene/ao_generate.hlsl and ao_apply.hlsl; CPU reference `ao_generate`, `ao_blur`)

The scene depth is reverse-Z with an infinite far plane, so the view distance of a pixel is exactly `near / depth` (0 is the sky).

**Pass 1, generate** (`AoGenerate`, writes the half-float `AoValue` target). For each pixel:

1. Rebuild the view-space position from the depth and the pixel center, and the normal from the neighbouring depths: the forward difference on whichever side has the smaller depth step (so a silhouette does not
   bend it), `cross(dy, dx)` facing the camera.
2. Project `radius` to pixels (`radius * height / (2 tan(fov/2)) / z`), clamped to 48 px. A radius under a pixel rounds every tap onto the pixel itself, which occludes nothing.
3. Twelve taps on a golden-angle spiral whose radius grows with the tap index, rotated per pixel by a 4x4 ordered pattern (so the neighbours' taps interleave). A sky tap occludes nothing.
4. In the manner of Alchemy's ambient obscurance, each tap contributes `max(0, v.n - bias) * w * radius / (v.v + 0.01 radius^2)`, with `v` from the pixel to the tap and `w = saturate(1 - v.v / radius^2)` (taps
   beyond the radius count for nothing). The result is `saturate(1 - 2 intensity * sum / 12)`.

**Pass 2, apply** (`AoApply`, writes the `SceneHdrAo` target the tone map then reads). A 5x5 blur of the occlusion, weighted by `1 / (1 + dx^2 + dy^2)` and by `saturate(1 - |dz| / (0.5 radius))` (a neighbour across a
depth edge, or in the sky, contributes nothing: occlusion does not bleed over silhouettes), removes the 4x4 pattern; the scene color is multiplied by it. A sky pixel is copied unchanged.

Both passes read with `Load` only (no sampler). The CPU functions are the same arithmetic in float.

## Where it runs

Scene -> AoGenerate -> AoApply -> ToneMap -> output. The depth buffer gains `Sampled` usage and ends the frame in the sampled state (the next frame's Scene pass transitions it back); the renderer creates the
occlusion value target and the occluded color target with its other size-dependent targets (all-or-nothing on resize), at the cost of one half-float and one RGBA16F target. Bloom, FXAA and the other post effects
are separate branches off main, each touching the same section of `SceneRenderer::render`; combining them is an integration step (the intended order: scene -> AO -> bloom -> tone map -> FXAA).

## Limits (read before relying on it)

* **It multiplies the finished color.** The scene is lit in one forward pass, so the occlusion also darkens directly lit surfaces in creases, which an ambient-only term would not. It is an artistic crease/contact
  darkening, not a physically separated ambient term. Keeping it out of direct light needs a depth pre-pass or a deferred ambient (future work).
* It is screen space: it sees only what the depth buffer holds. Geometry off screen or hidden does not occlude; a thin object in front of a surface darkens it as if it were a wall within `radius`; a surface seen at
  a grazing angle has noisy normals.
* Twelve taps and a 5x5 blur: no temporal accumulation, so close inspection shows a faint residual pattern at high intensity.
* Software Vulkan evidence only; no physical-GPU timing is claimed.

## Verification

* `engine/render/tests/test_ambient_occlusion.cpp` (CPU): ranges; a mismatched or invalid view refused; a flat floor and the sky exactly open; a concave corner darkened between 0.2 and 0.7 at the crease and fully open
  away from it, monotonically; intensity 0 is nothing, and the effect grows with intensity and radius and shrinks with bias; a convex pillar face stays open while its base darkens; determinism; a sub-pixel radius leaves
  everything open and a huge one stays in [0, 1]; the blur averages over a surface but not across a depth edge or from the sky; pass construction failure injection at every acquisition.
* `engine/render/tests/test_scene_renderer.cpp` (null device): refusals, no pass or resource when off, two extra draws and the targets on, resize without a leak, failure injection during resize.
* `engine/rhi_vulkan/tests/test_ambient_occlusion.cpp` (validated Vulkan with synchronization validation, lavapipe in CI): a cube on the ground rendered with and without the pass; the GPU's per-pixel color ratio against
  the CPU reference computed from the scene's analytic depth (12458 pixels: mean difference 0.0002, none beyond 0.08; 227 pixels darkened on the GPU against 223 in the reference), the sky unchanged, nothing brightened,
  the second frame identical (the sampled-depth state round-trips), zero intensity bit-identical to off, a larger radius darkens more pixels.
* Mutation checking (the CPU reference, both shaders and the renderer wiring, 31 mutants): every behavioral mutant fails a test. The survivors are the five **sky guards** (a sky tap, a sky neighbour in the blur, the
  sky pixel early-out in the apply pass): in exact arithmetic they change nothing (a sky position is 10^6 m away, so its tap weight is exactly 0, and a sky centre has no depth neighbour within the blur's depth range), so
  they are kept as robustness against infinite intermediate values, not as behavior. Pattern, blur-weight and tap mistakes in the shader that the aggregate GPU comparison could hide (0.07 to 0.14 worst error)
  are caught by its worst-case and tail thresholds.
