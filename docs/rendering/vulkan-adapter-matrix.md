# Vulkan adapter matrix

What the Vulkan adapter (`engine/rhi_vulkan`) is built against, what it needs at run time, and how newer Vulkan versions are
adopted without raising the baseline. Companion to the Vulkan 1.4 and PBR evaluation (branch `claude/vulkan-1.4-pbr-evaluation`, not yet on main) and
issue #43. The runtime requirement is Vulkan 1.3 and stays 1.3: building against newer headers changes what the compiler can
describe, never what the adapter demands.

## 1. Build-time and CI components

| Component | Before | Now | How it is selected | Notes |
|---|---|---|---|---|
| Vulkan-Headers | pin `v1.3.296`, **but only when no system headers were found** | `vulkan-sdk-1.4.363.0`, **fetched by its full commit** `6802bb4733b63ed5efd3adb308a6c885ef180ea1` (the tag resolves to it; a tag can move, a commit cannot), minimum accepted `VK_HEADER_VERSION` 303 | installed headers if a translation unit including that exact directory compiles with the Vulkan 1.4 API, else fetched (`SAIGE_VULKAN_FETCH_HEADERS=ON`), else a configure error | The release is the newest SDK-aligned tag in the repository at the time of writing (`git ls-remote`); `v1.4.369` is newer but not an SDK release |
| Loader (`vulkan-1.dll`, `libvulkan.so.1`) | opened at run time | unchanged | OS or SDK install | No link dependency; the adapter negotiates the instance version with whatever the loader offers |
| Validation layer | distro/msys2 package | unchanged | CI installs it | Not upgraded here: that means a new download source in CI. See section 4 |
| Lavapipe (software ICD) | Linux: distro Mesa; Windows: pinned Mesa 26.3.0 (SHA-256 checked) | unchanged | CI | Reports Vulkan 1.4 in both lanes, which is what makes a 1.4 path testable without a physical GPU |
| glslang | `saige-shaderc`; SPIR-V embedded as generated headers | unchanged | `saige_regenerate_shaders` | Shaders do not depend on the Vulkan header version |
| stb_image, nlohmann/json | pinned with hashes | unchanged | FetchContent | Out of scope |

### What the header change surfaced

- **The old pin was dormant on Linux.** `find_package(Vulkan)` found the distro's `libvulkan-dev` (headers 1.3.275 in the Ubuntu 24.04 package, confirmed on a machine with the same package), so
  `v1.3.296` was only ever used by the Windows lanes. Linux compiled against different headers than Windows. The Linux lane now
  fetches the pinned headers explicitly, so every lane builds against the same ones.
- **The version check inspects the tree that is used (review of #44).** An earlier revision read `VK_HEADER_VERSION` from one
  `vulkan_core.h` and then let `find_package` select an include tree, possibly a different one, and a patch number alone does not
  prove the 1.4 API is there. `cmake/SaigeVulkanHeaders.cmake` now compiles a probe against the exact directory the build will use
  (`VK_VERSION_1_4`, the minimum `VK_HEADER_VERSION`, `VkPhysicalDeviceVulkan14Features`), for installed headers and for the
  fetched ones, and the build is given that same directory through its own interface target (no `find_package`, so no clash with the
  fetched `Vulkan::Headers`). Three CTest cases run the probe on stand-in header sets: one that predates 1.4, one whose patch number
  looks new but that lacks the API (the old check would have accepted it), and a good one.
- **Fetched headers must be system includes.** Khronos's version macros use C-style casts, which this project's
  `-Wold-style-cast -Werror` rejects when the include path is not a system path (installed SDK headers always are). The fetched
  target is marked `SYSTEM`.
- **No source change was needed.** The adapter compiles unchanged with `-Werror` on 1.4.363 headers, and the full suite passes
  (38/38 with Vulkan and validation on llvmpipe). Windows MSVC `/W4 /WX` is checked by CI on the branch, not locally.
- **Probing by path, not by target.** The fetched `Vulkan::Headers` cannot coexist with the imported target `FindVulkan`
  creates, so the version is read from `vulkan_core.h` before choosing a source.

## 2. Runtime negotiation (the 1.3 fallback)

Three independent versions exist and must not be conflated: the **loader's** instance version, the **instance** version the
adapter requests, and each **physical device's** `apiVersion`.

| Question | Rule |
|---|---|
| What instance version is requested? | `min(loader, highest version SAIGE implements)`. Default stays 1.3 until a 1.4 path ships and is reviewed. |
| What is a device's effective version? | `min(instance version, device.apiVersion)`. A 1.4 loader with a 1.3 device is a 1.3 device. |
| When is a 1.4-only feature used? | Only if the effective version is 1.4 **and** the feature bit is set, **or** the equivalent promoted extension is enabled on a 1.3 device and its feature bit is set. Never on version alone. |
| What if a 1.4 feature is missing? | The adapter uses the 1.3 path. Absence is data (`DeviceInfo`), never an error. |
| What raises the floor above 1.3? | A review decision backed by measured benefit on physical hardware (#9) and driver-availability data. Not this document. |

The negotiation is a pure function of (loader version, requested cap, device version, extension list, feature bits), so it is
unit-tested without a device, including: loader 1.3 with a 1.4 cap; loader 1.4 with a 1.3 device; 1.4 device with a feature bit
clear; promoted extension present on a 1.3 device. Each is a deterministic fallback case. The Vulkan lanes then run the same
scene and diagnostics under the forced-1.3 path and the 1.4 path on Lavapipe, and report software and hardware observations
separately.

## 3. Promoted features to probe first (read-only)

Probe and report; enable nothing until a feature has a user. Available through core 1.4 or the extension on 1.3 headers:

| Feature | Core in | Extension | Used for | First user |
|---|---|---|---|---|
| Push descriptors | 1.4 | `VK_KHR_push_descriptor` | per-draw material without pooled sets | none yet; only after a measurement |
| Dynamic rendering local read | 1.4 | `VK_KHR_dynamic_rendering_local_read` | G-buffer reads without subpasses | later deferred or visibility-buffer phase |
| Host image copy | 1.4 (optional functionality) | `VK_EXT_host_image_copy` | upload without a staging buffer | `upload_texture` fast path |
| Maintenance 5 / 6 | 1.4 | `VK_KHR_maintenance5/6` | sized index binds, `vkCmdBindDescriptorSets2` | when the baseline moves |
| Scalar block layout | 1.4 (required) | `VK_EXT_scalar_block_layout` | tightly packed material structs shared with HLSL | PBR material buffers |
| Index type uint8, line rasterization | 1.4 | `VK_KHR_index_type_uint8`, `VK_KHR_line_rasterization` | small meshes, debug lines | later |

The requirement behind each row must be checked against the normative specification before the feature is enabled; the
promotion list above comes from the Vulkan 1.4 proposal document, and limits such as a push-constant minimum are not asserted
here.

## 4. Not done in this change, and why

- **Validation layer / SDK upgrade in CI.** Both lanes install the layer from a package manager. Moving to a LunarG SDK
  download adds an external dependency, hash pinning and a failure mode (download availability) to every run. It is a separate
  decision; the layer that detects descriptor-read hazards is the reason to consider it. The current layers do not detect them,
  which the capability probe reports on every run.
- **Enabling any 1.4 feature or requesting a 1.4 instance by default.** The first step is the probe in section 2.
- **Physical-GPU data.** Everything here is software Vulkan or build-time. Hardware observations stay in #9.
