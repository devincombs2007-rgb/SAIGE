# Prepared models and the upload path

`gfx::stage_model` used to do all of its work in one call on the host's staging thread: decode the images, build the mip chains, pack the vertices, then create the GPU objects, which
waits on the device's transfers. A host that wants to stream an asset in without a frame hitch needs the first half on a worker and the second half where it chooses, with the cost of each
visible. This slice separates them without changing what staging produces.

```cpp
StageCapabilities caps = capabilities_of(renderer);                     // shading mode, skinning, alpha blending: what the payload depends on
PreparedModel prepared = prepare_model(asset, "name", caps, settings);   // CPU only: no renderer, no device, any thread, pure
GpuModel model = upload_model(renderer, prepared, &report, &seconds);    // GPU objects; the payload is not consumed
// stage_model(renderer, asset, name, settings, &report) == upload_model(renderer, prepare_model(asset, name, capabilities_of(renderer), settings), &report)
```

## What the payload is

A value: images as whole mip chains (per source image and color space), materials with their factors and the sampler description of each slot, vertices and indices packed to the exact
bytes that will be uploaded (`ModelVertex` or `SkinnedModelVertex`, 16- or 32-bit indices), bounds, placements, the skin's joint count and the report (warnings, counts). It refers to no device,
registry or renderer. `PreparedModel::cpu_bytes()` is the host memory it holds; `prepare_seconds` and `decode_seconds` are measured, so decode and mip cost is reported apart from the upload.

* **Immutable and reusable.** `upload_model` takes it by `const&` and leaves it alone. Uploading it twice gives two independent `GpuModel`s (a second renderer, or a re-upload); dropping one
  releases only its own objects. A payload outlives the renderer whose capabilities it was prepared for.
* **Per material mode.** Which slots are decoded, whether skinned vertices are packed and how blended materials are flagged depend on the renderer's configuration, so the payload records
  `StageCapabilities` and a renderer whose capabilities differ refuses it (`std::invalid_argument`, before creating anything). Preparing for several modes means preparing once per mode; the
  payload does not guess, and a Legacy payload carries one decoded image where a MetallicRoughness one of the same asset may carry two.
* **Failure.** `prepare_model` throws `std::runtime_error` naming the asset, mesh, image or material, and has no effect (it touches no shared state, so several threads may prepare at once; tested).
  `upload_model` is all-or-nothing on the device: a failure at any creation point releases everything made so far (swept at every creation point), never touches what the renderer was already
  showing, and leaves the payload usable for another attempt.

## What this slice does not do (the transfer ticket)

The RHI today uploads synchronously (`upload_texture` and buffer creation wait for the device), so `upload_model` still blocks the calling thread for the transfers. The agreed next step (Core's
#43 comment 7121748064) is a **graphics-owned transfer ticket** for unpublished resources, which this separation prepares for; its proposed contract, for review before any shared API exists:

* A ticket is created from a `PreparedModel` and a renderer, owns the staging memory and the destination resources until the device reports completion, **even if the host drops or supersedes
  it** (the resources are retired on the device's completion timeline, never freed under a running copy).
* `poll()` is non-blocking and reports `Pending`, `Ready` (the `GpuModel` can be taken, once) or `Failed(reason)`. A failed or canceled ticket never replaces an active binding: the host only ever
  swaps in a `GpuModel` it took from a `Ready` ticket, at a point of its choosing (Core's fixed-step activation).
* Admission is the host's: `bytes_to_upload()` (the payload's GPU bytes, known before submission) lets Core apply a per-step byte budget and a cancellation policy; the ticket does no scheduling of its own.
* It needs an RHI transfer path that does not wait inline (a transfer queue or deferred submission with a completion value); that is an RHI change and gets its own review.

## Verification

`engine/gfx_scene/tests/test_stage_model.cpp` (null device; the Vulkan staging tests pass unchanged): preparation without a device and its accounting, prepare+upload equal to `stage_model` for every
material mode (live objects, bytes, names, warnings, counts, bounds), a payload uploaded twice and into another renderer after the first is gone, capability mismatches refused with nothing created,
asset errors with the same messages, an upload failure swept over every creation point (8 here) with the payload reusable afterwards, four threads preparing the same asset to identical payloads,
skinned payloads, and the texture description, sampler and bounds contents. Twelve mutants of the new code (the capability check, the image cache, the report, texture counts, lazy creation,
index width, skinning, blending, default material, bounds, color space, samplers) each fail a test. Timing is printed for information only (an 8x8 fixture on a null device); no performance claim is
made, and nothing here is measured on a physical GPU.
