# SSR (SAIGE Super Resolution) — Architecture Specification (Build 002)

## 1. Overview & Design Goals
SSR 0.2 is a standalone, C++20 / Vulkan 1.4 temporal super-resolution subsystem for **SAIGE (Sensational Artificial Intelligent Game Engine)**. It upsamples jittered low-resolution HDR frames to native display resolution while preserving subpixel detail, suppressing temporal flicker, and preventing ghosting on disoccluded or reactive surfaces.

## 2. Pipeline Stages (Build 002)
1. **Input Validation & Sanitization:** Checks render/output dimensions, non-null pointers, and sanitizes `NaN`/`Inf` in HDR color, depth, and motion vectors (`sanitized_invalid_inputs` counter).
2. **3x3 Closest-Depth Motion Vector Dilation:** Selects the motion vector from the closest depth sample in the 3x3 render-resolution neighborhood (supporting both reversed-Z and standard-Z) to prevent foreground silhouette erosion.
3. **Depth-Based Disocclusion Detection:** Reprojects `history_depth_` using dilated motion vectors and compares against `center_depth`. Pixels exceeding `disocclusion_depth_threshold` (default `0.05f`) are flagged disoccluded (`disoccluded_pixels`), resetting history confidence to `0.15f` and boosting current-frame blend weight (`alpha >= 0.85f`).
4. **Reversible HDR Tone Compression & YCoCg Variance Clipping:** Applies Karis/reversible luminance compression (`c / (1 + luma)`) prior to RGB-to-YCoCg conversion, computes 3x3 mean/standard deviation, and clips reprojected history with motion-adaptive `gamma` (`[1.00, 1.50]`).
5. **Confidence- & Mask-Aware Temporal Accumulation:** Combines `history_confidence_`, `reactive_mask`, and `transparency_mask` (weighted `0.7x`) with exposure-compensated (`pre_exposure / prev_exposure`) history, then persists **unsharpened** history (`history_color_`, `history_depth_`, `history_confidence_`).
6. **Separated Post-History RCAS Sharpening:** Applies a 5-tap edge-adaptive RCAS-style sharpening pass (`ssr_sharpen.comp` on GPU, loop in `ssr.cpp`) to the output buffer only, preventing sharpening feedback loops in temporal history.

## 3. Quality Presets
| Mode | Render Scale | 1080p Render Target | 4K Render Target |
| :--- | :--- | :--- | :--- |
| `NativeAA` | `1.00x` | `1920 x 1080` | `3840 x 2160` |
| `UltraQuality` | `0.77x` | `1478 x 832` | `2957 x 1663` |
| `Quality` | `0.67x` | `1286 x 724` | `2573 x 1447` |
| `Balanced` | `0.58x` | `1114 x 626` | `2227 x 1253` |
| `Performance` | `0.50x` | `960 x 540` | `1920 x 1080` |
| `UltraPerformance` | `0.33x` | `634 x 356` | `1267 x 713` |

## 4. Vulkan 1.4 Compute Shaders
- `shaders/ssr_reconstruct.comp`: GLSL 460 compute shader (`local_size_x = 8, local_size_y = 8`) with optional `SSR_USE_FP16` (`GL_EXT_shader_16bit_storage` / `GL_EXT_shader_explicit_arithmetic_types_float16`), ping-pong `history_color`, `history_depth`, and `history_confidence` images, and `transparency_mask` support.
- `shaders/ssr_sharpen.comp`: Separated post-history RCAS sharpening pass reading `u_accum_in` and writing `u_final_out`.
- **Execution Status on Delivery VM:** SPIR-V compilation and Vulkan validation-layer execution are **BLOCKED** on the delivery VM because `glslangValidator`, `glslc`, `spirv-val`, and `<vulkan/vulkan.h>` are not installed (only `libvulkan.so.1` and `libvulkan_lvp.so` runtime libraries exist). See `gpu/ssr_vulkan_harness.hpp/.cpp` for the reproducible host-side Vulkan 1.4 test harness.
