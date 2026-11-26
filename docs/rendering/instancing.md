# Instanced model draws

Status (2026-11-26): implemented on `claude/fidelity-dev`, opt-in, off by default. Covers rigid, opaque primitives in the scene pass. Correctness only: no frame time here is a hardware measurement.

## What it does

`ModelDraw` gains `instances` / `instance_count`. With `instance_count == 0` (the default) nothing changes. Otherwise the model is drawn once per matrix, each instance placed at `world * instances[i]` (`world` is a parent transform shared by all).

`RendererSettings::instancing` (`ModelRenderSettings::instancing`) turns on the batched path. It creates one more vertex stage (`shaders/scene/model_instanced.hlsl`, the rigid vertex stage of `model.hlsl` reading its world matrix from a storage buffer indexed by `SV_InstanceID`, which includes the draw's first
instance), four more pipelines (mirror state x double-sided), and a ring of `DeviceInfo::frames_in_flight` host-visible storage buffers of `max_instances` entries (48 bytes each), with one bind group per slot at set 2. The pixel stage is whichever of the four model permutations (sun shadows, image-based lighting) the renderer already uses.
`model.hlsl` and its generated headers are untouched.

For each placement whose primitives are all rigid and opaque, the renderer writes the instances that pass frustum culling into the frame's buffer, once per primitive and per mirror state (a mirrored instance needs the opposite winding, so it is drawn by its own call), and issues **one `draw_indexed` per primitive and mirror state** with the instance count and first instance. The result is
pixel-identical to the same instances drawn as separate `ModelDraw`s.

## What stays instance by instance

* Everything, when `instancing` is off (an instanced `ModelDraw` is harmless there; it allocates nothing).
* A placement with a skinned primitive (one palette per draw), or a blended one (the sorted phase orders each primitive separately).
* The shadow pass (`draw_shadow`): its depth shaders have no instanced variant yet.
* A placement that does not fit what is left of the frame's buffer: `ModelFrameStats::instance_overflow` counts it, and it is drawn instance by instance (the check is conservative: `instance_count x primitives`, so a placement is batched whole or not at all).

Order: an instanced draw is placement by placement, then instance by instance (a group of separate `ModelDraw`s is instance by instance); opaque depth-tested draws commute, so the image does not depend on it.

## Counters

`ModelFrameStats` / `FrameStats`: `instanced_draws` (batched calls, also in `draw_calls`), `instances_batched` (the instance-primitive pairs they covered, i.e. the calls the frame would have needed), `instance_overflow`. Existing counters keep their meaning: `instances_drawn` counts placement-instances, `instances_skipped` degenerate matrices, `primitives_culled` culled primitive-instance pairs.

## Tests (`engine/rhi_vulkan/tests/test_instancing.cpp`, lavapipe, synchronization validation)

Batched vs separate draws vs the fallback give identical images (twelve cubes with different scales and rotations, two mirrored); the draw-call saving is exactly ten; frustum culling drops the same instances; a NaN matrix is skipped and counted; sun shadows on; blended primitives are not batched; a buffer too small for a placement falls back and reports it, while an exact fit batches;
five back-to-back frames reuse the ring without overflow; invalid use is rejected. Thirteen deliberate defects were each caught (mirror sign, mirrored pipeline, first instance, transposed rows, cursor advance and per-frame reset, batched culling, an off-by-one in the capacity check, batching blended primitives, the batched count, mirror grouping, degenerate instances, `instances_drawn`).
The existing Vulkan suite (175 tests) passes unchanged.

## Not done

Instanced shadow depth shaders; skinned or blended batching; GPU-driven culling (the next item: a compute pass that writes the visible instances and indirect arguments, checked against the CPU culling above); any measurement on a physical GPU.
