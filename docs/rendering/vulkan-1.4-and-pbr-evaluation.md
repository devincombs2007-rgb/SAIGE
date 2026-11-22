# Vulkan 1.4 adoption and the next PBR milestone: technical evaluation

Status: proposal for owner and Core review. Written after the 0.2 renderer stack was complete and under review; nothing here
changes 0.2 scope, and nothing here is implemented. Evidence labels: **[code]** read from this repository, **[sources]**
from the pages linked at the end, **[unverified]** a claim I could not confirm and which must be checked before relying on it.

## 1. Where we are

- The backend requires Vulkan 1.3 [code: `vulkan_device.cpp` rejects loaders and devices below 1.3] and four features:
  `dynamicRendering`, `synchronization2`, `timelineSemaphore`, and `shaderDemoteToHelperInvocation` (mandatory in 1.3; HLSL
  `discard` needs it). The instance is created with `apiVersion = 1.3`.
- Software Vulkan already reports 1.4 on both CI platforms (lavapipe/llvmpipe appear as "Vulkan 1.4" in the Windows lane log),
  so a 1.4 code path can be exercised in CI without a physical GPU. Physical-GPU verification is still #9 and nothing below
  has been measured on one.
- Draws use a 128-byte push-constant block and per-material bind groups backed by pooled plain descriptor sets. The
  bindless allocator exists as a CPU-side design but the renderer does not use it yet.

## 2. What Vulkan 1.4 offers us

Promoted to core in 1.4 [sources]: push descriptors, dynamic rendering local read, `maintenance5` and `maintenance6`, host
image copy (functionality optional), index type uint8, line rasterization, vertex attribute divisor, load/store op none,
shader subgroup rotate, float controls2, expect/assume, global priority, pipeline robustness. Scalar block layout is
mandatory. `maxColorAttachments` rises to 8 (Roadmap 2024). A push-constant limit increase to 256 bytes is something I
remember but could not confirm in the search results: **[unverified]**, check the spec's Limits table before designing for it.

| 1.4 item | Use for SAIGE | Value now | Verdict |
|---|---|---|---|
| Push descriptors | Per-draw material without allocating or pooling sets | Low: our groups are created at load and pooled; no measured cost | Optional path, adopt only if a measurement shows set binding on a hot path |
| Dynamic rendering local read | G-buffer/visibility reads without subpasses | None on desktop forward; relevant to a later deferred or visibility-buffer resolve on tile GPUs | Defer to Phase 3 |
| Host image copy | Upload textures without a staging buffer | Medium: simplifies `upload_texture` and cuts a copy, but optional per device so the staged path stays | Add as capability-gated fast path |
| maintenance5/6 | Sized index-buffer binds, `vkCmdBindDescriptorSets2`, robustness fixes | Low-medium | Take for free when we raise the baseline |
| Scalar block layout | Tightly packed material and light structs shared with HLSL | Medium for PBR material buffers | Useful at 1.4; at 1.3 it is a feature we can already query |
| Line rasterization | Debug lines, gizmos | Low | Later |
| Subgroup rotate, float controls2 | Compute passes (clouds, culling) | Low until compute exists | Later |

The honest summary: nothing in 1.4 unblocks a milestone we are planning. The gain is mostly simplification and a cleaner
feature floor, and the cost is a narrower hardware set.

## 3. Cost and risk of requiring 1.4

- **Hardware reach.** NVIDIA shipped conformant 1.4 drivers at launch on Windows and Linux; Mesa's NVK, ANV, RADV, Turnip and
  Honeykrisp were conformant on day zero; AMD and Intel stated their Windows drivers would follow "early" the next year
  [sources]. I did not find a current per-vendor Windows driver matrix or any install-base figure, so how many of our
  players could run a 1.4-only build is **[unverified]**. Raising the floor can silently exclude older GPUs that run 1.3
  fine.
- **Code.** Small: add the 1.4 entry points to the X-macro table in `vk_api.hpp`, chain `VkPhysicalDeviceVulkan14Features`,
  and surface the result in `DeviceInfo`. The function-table design already isolates this.
- **Test.** Two lanes already exist that report 1.4 (lavapipe). A 1.3-capped run needs one more lane or an env override so
  the fallback paths are not untested code.

## 4. Recommendation: stay on 1.3 as the floor, add 1.4 as a detected tier

1. Keep 1.3 as the **required** baseline through 0.2 and the PBR milestone.
2. Create the instance at the highest version the loader offers (still validating 1.3 as the minimum), query the promoted
   features, and expose them as booleans on `DeviceInfo` (`push_descriptors`, `host_image_copy`, ...). Gate each fast path
   behind its boolean and keep the 1.3 path as the tested reference. No call site may branch on the API version.
3. Add a CI switch that caps the device at 1.3 so both tiers run in lavapipe lanes.
4. Revisit the floor only when both hold: (a) a measured win on physical hardware from a 1.4-only feature, and (b) driver
   availability data that the owner accepts. Until #9 produces hardware numbers, (a) cannot be shown and I would not change
   the floor.

Upgrade posture beyond 1.4: treat future Roadmap profiles the same way (capability tiers). The SDK now ships a Roadmap 2026
profile and Mesa 26.0 reportedly incorporates it [sources]; adopting a profile as a floor is a product decision, not a technical one.

## 5. Next milestone: forward PBR with image-based lighting (proposed 0.3)

This is Phase 2 of [render-architecture.md](render-architecture.md): the simplest correct baseline, used to validate materials
and assets before GPU-driven rendering.

**Scope**
- glTF metallic-roughness: base color, metallic-roughness, normal, occlusion and emissive textures, with the glTF
  `KHR_materials_emissive_strength` and `KHR_texture_transform` extensions as the only extensions. 0.2 samples base color
  only, so the other slots currently produce importer warnings.
- Tangents: use the asset's tangents when present; otherwise generate MikkTSpace tangents in the asset pipeline (CPU,
  deterministic), never in the shader.
- Linear HDR pipeline: scene in `RGBA16_FLOAT`, one tone-mapping pass (pick one operator and document it), exposure as a
  parameter, sRGB only at the swapchain.
- Lighting: one directional sun with cascaded shadow maps, a prefiltered environment (split-sum) plus BRDF LUT for image-based
  lighting. Alpha blend with simple back-to-front sorting; order-independent transparency is out of scope.
- Reuse what exists: material bind groups, the registry, the staging and activation path, GPU timestamps and memory
  diagnostics, and the `RendererDiagnostics` schema.

**RHI prerequisites (the real cost of the milestone)**: compute pipelines and storage images (environment prefilter, BRDF
LUT, later clouds and culling), cube and 3D textures, GPU mip generation, depth-only and array render targets for cascades,
and buffer device address (core since 1.2) so ray tracing and GPU-driven passes need no redesign. Each is additive to the
current RHI; none changes an existing signature.

**Verification, in the style 0.2 established**
- A CPU reference for the BRDF (GGX, Smith, Schlick) and image-based terms, compared with rendered pixels on constant-color
  and furnace scenes; a white-furnace test (energy conservation within a stated tolerance) catches most BRDF mistakes.
- Khronos glTF sample assets for materials (metal/rough spheres, normal and tangent tests), compared against reference images
  with an explicit tolerance; the reference images are checked in with provenance.
- Mutation checks on the shader math, as with the lighting oracle: a wrong Fresnel or a swapped roughness must fail a test.
- Validation and synchronization validation on every pass (the new passes are the highest hazard risk); GPU pass timings
  reported as unavailable on software devices, measured only on physical hardware (#9).

**What it preserves for later phases**
- Ray tracing: acceleration-structure inputs need vertex and index buffers with device-address and storage usage; the model
  buffers should be created with those usage flags in this milestone so no re-upload is needed later (they do not have them
  today).
- Virtualized geometry and GPU-driven culling: the visibility-buffer phase reuses the same material bind groups and the same
  diagnostics; the bindless allocator replaces per-material sets without touching the registry.
- Volumetric clouds: need the compute and storage-image prerequisites above, which this milestone builds for IBL.

**Decisions I need from the owner and Core**
1. Confirm the capability-tier policy in section 4 (1.3 floor, 1.4 detected).
2. Confirm 0.3 scope: forward PBR plus IBL plus one cascaded sun, no transparency beyond sorted blend.
3. Core: agree on a material-parameter contract for the project format (which glTF extensions the saved scene may rely on).
4. Prioritize physical-GPU access (#9) before any performance claim; every number in this repository today is a software
   rasterizer's CPU path.

## Sources

- [Vulkan 1.4 proposal (docs.vulkan.org)](https://docs.vulkan.org/features/latest/features/proposals/VK_VERSION_1_4.html)
- [Core Revisions (Informative)](https://docs.vulkan.org/spec/latest/appendices/versions.html)
- [GamingOnLinux: Vulkan 1.4 released with new required extensions](https://gamingonlinux.com/2024/12/vulkan-api-14-released-with-new-required-extensions-better-8k-rendering)
- [CNX Software: Vulkan 1.4 released](https://www.cnx-software.com/2024/12/04/vulkan-1-4-3d-graphics-and-compute-api-released/?amp=1&noamp=mobile)
- [Collabora: NVK now supports Vulkan 1.4](https://www.collabora.com/news-and-blog/news-and-events/nvk-now-supports-vulkan-14.html)
- [Vulkan SDK 1.4.341.1 release notes (Roadmap 2026 profile)](https://ftp.fi.muni.cz/pub/linux/slackware/slackware64-current/source/x/vulkan-sdk/release_notes.html)
