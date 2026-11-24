# Sorted alpha blending

glTF `alphaMode: BLEND` materials are composited source-over with straight (non-premultiplied) alpha in a phase after all opaque
and masked draws, **when the host asks for it**: `RendererSettings::alpha_blending` (`ModelRenderSettings::alpha_blending`) is off by
default, and with it off a BLEND material draws opaque exactly as in 0.2, so saved 0.2 scenes look as they did (review of #59). Turning
it on is an owner decision about defaults. Built on the RHI blend state (`saige/rhi/blend.hpp`, docs/rendering/rhi-design.md section 6.1). Owner approval:
0.3 review (sort key, two-pass double-sided, documented limits, tests before integration).

## Behavior

- `MaterialData::alpha_blend` (set by `stage_model` for `BLEND` materials) makes `ModelRenderer::create_material`, when alpha blending is
  enabled, mark the
  `GpuMaterial` as blended and create the four blend pipelines on first use (mirrored x back-face-culled and front-face-culled
  variants; never for scenes that have no blended material).
- A blended pipeline is the model pipeline plus `BlendState::source_over()` and depth state *test on, write off* (reverse-Z
  greater): blended surfaces are hidden by opaque ones in front of them and never hide anything themselves.
- The shader outputs `saturate(base color alpha)` (base color factor times texture alpha) for blended materials and 1 for all others;
  fog mixes the color only, so a faded surface fades into the fogged scene behind it. Lit color, specular and emission are all
  composited with that alpha, as glTF's painter's-algorithm description requires. Works in Legacy and metallic-roughness shading.
- HDR: compositing happens in the RGBA16_FLOAT scene target in linear light, then the tone map runs. (Blending already tone-mapped
  colors gives a different result; a test pins this.) LDR: blending into the sRGB target is linear (the hardware decodes and encodes).
- **Sorting:** within `ModelRenderer::draw`, opaque and masked primitives draw first in submission order. Blended primitives are
  collected and sorted back to front by `blend_sort_depth`, the view-space depth of the world-space center of the primitive's
  bounding sphere (the clip-space w of `view_proj * center`, for a perspective camera), with submission order
  (draw, placement, primitive) as the tie-break, so a frame is reproducible. `GpuPrimitive::bounds_center` and `bounds_radius` are
  filled by `stage_model` (box center, farthest vertex); hand-built models that leave the default sort by their model's origin.
- **Double-sided** blended primitives draw twice, back faces first (front-face culling) and then front faces (back-face culling),
  so a convex mesh composites correctly against itself. Single-sided ones draw their front faces once. Mirrored instances use the
  clockwise-winding variants, so the same faces are chosen.
- Frame statistics: `FrameStats::model_blended_primitives`, `ModelFrameStats::blended_primitives` and `blended_draws` (two per
  double-sided primitive).
- Fallback: with `alpha_blending` off (the default), or if the scene format is not in the portable blend subset (for example 32-bit
  float), blended materials draw opaque and staging warns once, saying which. UNORM, sRGB and 16-bit float formats, which includes both the output formats and the HDR target, blend.

## Limitations (by design, stated plainly)

- Sorting is **per primitive**, by one depth value each. Transparent triangles that interpenetrate, mutually overlap or wrap around
  each other inside one primitive, and large primitives whose centers order differently from their surfaces (a big ground-like
  quad against small objects above it), can sort wrongly. The two face passes fix a single convex mesh against itself, not
  concave or self-intersecting ones.
- No order-independent transparency, no refraction, no `KHR_materials_transmission` or `volume`, no per-triangle sorting.
- The key assumes a perspective projection (orthographic cameras would give every primitive the same depth). The renderer has
  perspective cameras only.
- Blended primitives are not culled or batched; each is its own draw (two if double-sided).
- Blended materials write alpha into the target's alpha channel; the output formats do not use it.

## Tests

`engine/rhi_vulkan/tests/test_blend_render.cpp` (validated Vulkan device, synchronization validation on, lavapipe in CI): the sort
key (view depth, not distance), source-over against the equation in LDR/HDR and Legacy/metallic-roughness for alpha 0, 0.25, 0.5, 1,
all six submission orders of three layers giving identical bytes equal to the back-to-front composite, sorting through the world
matrix, opaque occluding blended and blended blending with opaque, no depth write (coplanar blended quads both contribute), a
double-sided two-layer surface drawn back faces first (a single unsorted pass in index order would give a different color), mirrored
instances, eight frames in flight in LDR and HDR, and 1000 sorted primitives. `engine/render/tests/test_model_blend.cpp`
(null device): which scene formats blend, the opaque fallback, lazy and leak-free pipeline creation. Mutations that each fail
the suite: no sort, near-to-far sort, depth write on, blending off, single pass, passes swapped, reversed tie-break, bounds center
ignored, world matrix ignored, alpha forced to 1, eager pipeline creation.
