# GPU frustum culling of instances

Status (2026-11-26): implemented as a standalone, tested building block on `claude/fidelity-dev` (`saige::render::InstanceCuller`). **Not yet wired into `ModelRenderer`.** Correctness only, on lavapipe; no performance claim and nothing here is physical-GPU evidence.

## What it does

`InstanceCuller::cull` records a compute pass (`shaders/scene/cull_instances.hlsl`, 64 threads per workgroup) over the world matrices of the instances of one primitive. Each instance's bounding sphere (a local centre and radius shared by all instances, as `GpuPrimitive::bounds_center` and `bounds_radius`) is carried to world space by
the affine matrix, its radius scaled by the instance's largest axis scale, and tested against the six frustum planes **exactly as the CPU's `intersects_sphere` does** (the planes are the CPU `Frustum`'s, uploaded as they are: culled when the signed distance to any plane is below minus the radius; a radius of zero is never culled). The survivors append their matrix rows (three float4) to a compacted
buffer. The append position is an atomic counter that lives in the indirect draw arguments themselves (the instance count), so the draw that follows needs no readback: the host initialises the arguments to `{index_count, 0, 0, 0, 0}` and the pass fills in the count. The pass ends with the barriers that make the rows visible to the vertex stage and the arguments to the indirect draw.
`draw_indexed_indirect(arguments(), 0, 1, 0)` then draws exactly the visible instances.

The order of the compacted instances is undefined (atomic arrival), which is harmless for opaque, depth-tested draws.

## Contract and limits

* Matrices must be finite, non-degenerate affines: the caller validates them, as the CPU path does before it culls.
* One `cull` per culler per frame, outside a rendering scope; the buffers are not versioned per frame in flight (the host writes the inputs and arguments when `cull` is called). A frame-slot ring and one culler per primitive are the integration step, together with an instanced vertex stage that reads the compacted rows.
* The arguments live in host-visible memory (20 bytes); the compacted rows are device-local unless `InstanceCullerSettings::host_visible_outputs` (for tests and tools: there is no buffer-to-buffer copy in the RHI to read them back otherwise).
* Needs `DeviceInfo::compute`; an indirect draw with a non-zero first instance would need `draw_indirect_first_instance`, but this pass writes a first instance of 0.
* A sphere within a hair of a plane can round differently on the GPU than on the CPU (the dot products may fuse differently); the random test scene leaves such instances out by a margin of 0.03 and says so.

## Tests (`engine/rhi_vulkan/tests/test_gpu_culling.cpp`, lavapipe, synchronization validation requested)

The compacted set equals the CPU test's set (600 random instances with rotations, non-uniform and mirrored scales; the arguments carry the count and pass the index count through); edge cases (nothing, everything, a radius of zero with every plane rejecting, a partial workgroup, exactly one workgroup, exactly the capacity); the instance scale grows the sphere as on the CPU; an
indirect draw of the compacted instances gives the same image as drawing the CPU-culled list directly (and differs from drawing everything); a second cull on the same culler starts from an empty draw; invalid use is rejected before anything is recorded.

Mutation checks: fifteen deliberate defects (the radius ignored in the plane test, the scale ignored or taken as the smallest axis, the centre without translation, a plane skipped, radius zero culled, the instance-count guard removed, every instance written to one slot, the counter in the wrong argument word, nothing visible, plane offsets negated on upload, too few workgroups, the arguments not re-initialised, the index count not passed through, and the barriers removed).
Fourteen were caught. **One survives: removing the barriers.** On lavapipe the validation layer cannot see shader (descriptor) reads, which is the same limit `Vulkan bind groups: report whether this validation layer can detect shader-read hazards` reports, and a CPU implementation executes the dispatch before the draw regardless; the barriers are justified by the Vulkan specification and the stage and access masks, not shown by a failing test here. A run on a physical GPU with the validation layers would be the check.

## Not done

Integration into the model renderer (per-primitive culling feeding the instanced draw), a frame-slot ring, hierarchical or occlusion culling, LOD selection on the GPU, and any measurement on a physical GPU.
