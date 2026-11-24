# GPU skinning

Linear blend skinning of imported glTF meshes on the GPU, opt-in, with the CPU reference in `gfx_assets/animation.hpp` as the parity oracle.

## Who owns what (agreed on #43)

| Layer | Owns |
| --- | --- |
| `gfx_assets` (engine evaluation) | stateless sampling: `evaluate_clip`, `skin_matrices`, `skin_vertex` (the CPU reference). No clock, no playback state. |
| Gameplay (Core) | playback time, blending, layering, events and state transitions; the pose it publishes for a frame is final for that frame. |
| `render` | the GPU copy of the pose (`SkinPalette`), the skinned vertex format, the skinned pipelines. It never evaluates animation. |

The renderer's input is the final **skin matrices** of the frame: for each joint, the joint's world transform times its inverse bind matrix, in the skinned
mesh's own space (what `gfx::skin_matrices(skin, pose)` returns). The instance's world matrix (`ModelDraw::world`) then places the skinned mesh as for any
rigid model. glTF says a skinned mesh node's own transform is ignored, so a skinned primitive's placement must be the identity.

**Not in this slice: importer staging.** `gfx_scene`'s `stage_model` does not yet build `SkinnedModelVertex` buffers from `gfx::Primitive::joints/weights`, set `skinned`/`joint_limit`, or place skinned instances at identity; models are hand-built in the tests. That is the next slice (it needs no renderer change).

## Using it

```cpp
RendererSettings rs;            // or ModelRenderSettings
rs.skinning = true;             // off by default: nothing is allocated for skinning when off
SceneRenderer renderer(device, format, rs);

SkinPalette palette = renderer.model_renderer().create_skin_palette(skin.joint_count(), "hero");   // one per animated instance
// every frame, after gameplay has produced the pose:
palette.update(gfx::skin_matrices(skin, pose));
const ModelDraw draws[] = {{&model, world, &palette}};
```

* A primitive is skinned when `GpuPrimitive::skinned` is set: its vertex buffer holds `SkinnedModelVertex` (a `ModelVertex` plus four `uint16` joints and four
  `float` weights, 72 bytes) and `joint_limit` is one more than the largest joint index it uses. Rigid primitives in the same model, and in the same frame, are
  unaffected and use the unchanged vertex path and pipelines.
* `ModelDraw::palette == nullptr` draws the bind pose (the renderer keeps an identity palette). Rigid primitives ignore the palette.
* One palette serves all skinned primitives of its instance, in the color pass, the sorted blended phase and the sun shadow pass.
* `SkinPalette::update` checks the pose (count, finite, affine) before changing anything, so a bad pose leaves the previous one. Calling it several times
  before a frame is the same as calling it once. The palette keeps the latest matrices on the CPU and a GPU copy per frame in flight; a draw rewrites the current
  slot if it is stale, so the GPU never reads a buffer the CPU is writing and a pose that is not updated stays current in every slot.

## What the shaders compute

`model_skinned.hlsl` is `model.hlsl` compiled with `SAIGE_SKINNING = 1`, vertex stage only; the pixel stage is whichever plain, shadowed, IBL or both permutation is in
use, because the vertex outputs are identical. With `B = sum_i w_i M_i`:

* position `B p`;
* tangent and bitangent (built from the unskinned normal and tangent as `cross(n, t) * handedness`) by the linear part of `B`;
* normal by the **cofactor** of that linear part, so normals stay perpendicular under non-uniform scale and under blends of several joints; the instance matrix's
  cofactor then composes (`cof(AB) = cof(A) cof(B)`).

`shadow_depth_skinned.hlsl` skins the position the same way for the shadow map. The palette is three `float4` rows per joint (affine matrices; the bottom row is
implicit), 256 joints at most (`kMaxSkinJoints`, 12 288 bytes, inside the portable 16 KiB uniform range). Joint indices are clamped to the palette in the shader and
checked against the palette's joint count at draw time (`ModelFrameStats::skinned_skipped`), so a corrupt or mismatched primitive never reads past the buffer.

The existing permutations' SPIR-V is byte-identical (only the source-hash lines of their headers changed): the skinned code is inside `#if SAIGE_SKINNING`.

## Limits

* Mirrored bones (a joint matrix with negative determinant) flip the triangle winding; mirror the instance (`ModelDraw::world`) instead.
* Normal-mapped tangent frames are exact for rotations, translations and uniform scale; under strong non-uniform bone scale the skinned bitangent (the linear
  part applied to the unskinned `cross(n, t)`) is not the cross product of the skinned normal and tangent. Positions and normals are exact in all cases.
* **Culling and bounds**: a skinned primitive's `bounds_*` are its bind-pose bounds. The frustum culling of #83 must use conservative animated bounds for
  skinned primitives (the union over the clip, or a per-instance bound supplied with the pose); skinned primitives must not be culled by bind-pose bounds. That
  belongs to the culling branch and is recorded there.
* Skins with more than 256 joints are not supported by this path (the importer's limit is 512). Morph targets, root motion, IK and retargeting are not part of
  this slice.
* Everything is software-Vulkan correctness; no physical-GPU timing is claimed.

## Verification

`engine/rhi_vulkan/tests/test_skinning.cpp` (validated Vulkan, lavapipe in CI) renders a skinned tube through the skinned pipelines and the same tube rigidly from the CPU
reference's vertices, and compares the images: across eight poses including a non-uniform scale and four influences per vertex (worst channel delta at most 1 8-bit level
in the shadow and IBL runs); the bind pose with no palette; six consecutive frames with a new pose each (more frames than frame slots), double updates and an
un-updated palette; two instances with different palettes in one frame with sun shadows and image-based lighting together (the skinned depth stage is exercised: the
floor's shadows match); sorted blended skinned primitives; a normal-mapped surface (the tangent path); and the refusals (skinning off, bad counts, a palette with too few
joints, NaN, non-affine and short poses). `engine/render/tests/test_scene_renderer.cpp` (null device): off allocates nothing, on adds its resources, a creation-failure
sweep over every acquisition step (with shadows and IBL) leaves the device at its baseline, and a palette creation sweep leaves nothing behind.
