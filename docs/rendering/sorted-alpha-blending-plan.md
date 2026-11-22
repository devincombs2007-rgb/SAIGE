# Sorted alpha blending: implementation plan

Owner decision 2 (0.3 approval): support proper sorted alpha blending rather than drawing Blend materials as opaque. This is a
plan. It needs a new RHI contract (blend state), so nothing is implemented until the combined renderer and the shared
decisions have been reviewed.

## Where things stand

- `AlphaMode::Blend` is parsed; `stage_model` warns "alpha blending is not implemented" and draws the material as opaque.
- `rhi::GraphicsPipelineDesc` has depth state but **no blend state**: the Vulkan backend creates every color attachment with
  blending disabled (`vulkan_device.cpp`, `VkPipelineColorBlendAttachmentState` with only a write mask). That is the new contract.
- `ModelRenderer::draw` issues one draw per primitive in the order given, with pipelines keyed by (mirrored, double-sided).
- `GpuPrimitive` has no bounds, so there is nothing to sort by yet.

## Semantics to implement (glTF)

Straight (non-premultiplied) alpha, source-over: `out = src.rgb * src.a + dst.rgb * (1 - src.a)`, with `src.a` = base color
factor times texture alpha. In glTF Blend mode alpha applies to the whole surface reflection (diffuse and specular), so the
shader output is the lit color with the existing fog mix and alpha unchanged; blending happens in the target's linear space, which
in HDR mode is RGBA16_FLOAT (blendable on every Vulkan implementation as a mandatory format feature) and before tone mapping.
RGBA8_SRGB output targets are also a mandatory blend format; blending then happens on linear values (the hardware decodes and
encodes), matching the oracle.

## Work in three independently reviewable steps

1. **RHI blend state** (RHI, null device, Vulkan; tests). `BlendState { bool enable; BlendFactor src_color, dst_color,
   src_alpha, dst_alpha; BlendOp color_op, alpha_op; }` per color attachment in `GraphicsPipelineDesc`, default disabled so
   every existing pipeline is unchanged. Vulkan maps to `VkPipelineColorBlendAttachmentState`. Tests: pipeline with blend
   creates and validates; a full-screen quad of alpha 0.25 over a cleared target reads back the exact source-over result for
   RGBA8_SRGB and RGBA16_FLOAT; default pipelines still overwrite. Decide with the owner whether blend state belongs in the
   RHI interface that Core also consumes: it is graphics-owned, but record it in the architecture document.
2. **Renderer** (render, gfx_scene, shaders; tests). Bounds on `GpuPrimitive` (center and radius, computed at staging from the
   positions); `GpuMaterial` records the alpha mode; blend pipeline variants (blend on, depth test on, **depth write off**, the same
   four mirrored/double-sided cull combinations); a sorted second phase inside the Scene pass after all opaque and masked draws;
   the shader writes alpha. Remove the staging warning.
3. **Documentation and diagnostics**: blended draw count in the frame stats and diagnostics JSON, gltf-subset.md, the
   architecture document.

## Sort policy

- Phase order: opaque, then alpha-masked (both depth-writing, order irrelevant), then blended primitives sorted **back to
  front**.
- Key: the view-space depth of the primitive's world-space bounding-sphere center (center transformed by the placement's world
  matrix, then by the view matrix; farther first). View-space depth rather than Euclidean distance, because the blend order
  has to agree with the depth ordering the rasterizer uses (distance can order two surfaces opposite to their depth).
- Deterministic tie-break: (draw index, placement index, primitive index) ascending, so equal keys keep submission order and a
  frame is reproducible and testable.
- Double-sided blended primitives draw in two passes, back faces (cull front) then front faces (cull back), the usual way to
  avoid the worst self-overlap artifacts of a single convex mesh. Single-sided: one pass.
- Known limits, stated up front: sorting is per primitive, so interpenetrating or mutually overlapping transparent triangles
  within one mesh, and large primitives that wrap around one another, can still sort wrongly. Order-independent transparency,
  refraction, and `KHR_materials_transmission`/`volume` are out of scope.

## Test plan (oracle and mutations)

1. Source-over against the CPU oracle: a quad of known color and alpha over known opaque content, LDR and HDR (ACES), single
   pixel compared in linear space; alpha 0, 0.25, 1.
2. Order independence: three overlapping blended quads at different depths; every permutation of the submission order gives the
   same bytes, and they equal the oracle's back-to-front composite. Mutation: disable the sort, the test must fail.
3. Opaque occludes blended: a blended quad behind an opaque one is hidden; in front it blends. Mutation: draw blended first.
4. Depth write off: two blended quads at the same depth both contribute; a blended quad does not occlude a later blended one.
5. Double-sided, mirrored instance: back-then-front result matches the oracle; mirror gives the mirror image.
6. Tie determinism: equal-depth primitives keep submission order across frames.
7. Combined with metallic-roughness, normal mapping and HDR in the integration test (`test_combined_pbr.cpp`).
8. Frames in flight with a failed or replaced scene still validation clean (sync validation).

## Decisions for the owner

- Sort key: view-space depth of the bounding-sphere center (recommended) versus nearest-point depth.
- Two-pass double-sided blending by default (recommended) or single pass.
- Whether the per-instance material override (decision 3) may change alpha mode; if so it changes the pipeline selection per
  draw. Schema stays Core's.
