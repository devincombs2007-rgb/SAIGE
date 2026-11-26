# SSR Engine Integration Contract (For Claude / SAIGE 0.3)

## 1. Alignment with SAIGE Architecture
Per [docs/architecture.md](https://github.com/devincombs2007-rgb/SAIGE/blob/claude/production-infrastructure/docs/architecture.md) and [GitHub Issue #43](https://github.com/devincombs2007-rgb/SAIGE/issues/43), SAIGE runs a `16,666,667 ns` (60 Hz) fixed-step `IGame::fixed_update` loop with an `IFrameSink` graphics adapter and Vulkan 1.4 / PBR HDR rendering (`pbr-m4-hdr-output`). SSR 0.2.1 is designed to drop into `IFrameSink` between the HDR opaque+transparent pass and final tone-mapping/UI compositing.

## 2. Required Engine Inputs (`SsrDispatchInputs`)
- `color_hdr` (`RGBA16F` / `RGBA32F` linear HDR): Rendered at `compute_render_resolution(out_w, out_h, mode)` with subpixel Halton jitter (`halton_jitter_px(frame_index)`).
- `depth` (`D32_SFLOAT` / `R32_SFLOAT`): Render-resolution depth buffer (`reversed_z = true` default).
- `motion_vectors_px` (`RG16F` / `RG32F`): Screen-space motion vectors in **render-resolution pixels** pointing from current pixel to previous-frame pixel (`prev_px - curr_px`).
- `reactive_mask` (`R8_UNORM` / `R32_SFLOAT`, optional): Particles, emissive VFX, and animated foliage requiring fast temporal response.
- `transparency_mask` (`R8_UNORM` / `R32_SFLOAT`, optional): Glass, water, and sorted alpha-blended surfaces (PR #59 `claude/sorted-alpha-blending`).
- `pre_exposure`: Current frame exposure multiplier (history is scaled by `curr_pre_exposure / prev_pre_exposure` before clipping).
- `camera_cut`: `true` on scene transitions, teleportation, or first frame to reset temporal history cleanly.

## 3. Descriptor Bindings, Push Constants, & Synchronization
- **`ssr_reconstruct.comp` (`Set 0`):**
  - Bindings `0..7` (`COMBINED_IMAGE_SAMPLER`): `u_color_hdr`, `u_depth`, `u_motion_px`, `u_reactive_mask`, `u_transparency_mask`, `u_history_color_in`, `u_history_depth_in`, `u_history_conf_in`.
  - Bindings `8..10` (`STORAGE_IMAGE`): `u_history_color_out` (`rgba16f`), `u_history_depth_out` (`r32f`), `u_history_conf_out` (`r16f`).
  - Push constants (`36` bytes): `uvec2 render_size`, `uvec2 output_size`, `vec2 jitter_px`, `float exposure_ratio`, `float disocclusion_threshold`, `uint flags`.
- **`ssr_sharpen.comp` (`Set 0`):**
  - Binding `0` (`COMBINED_IMAGE_SAMPLER`): `u_history_color` (unsharpened `history_color_out` from reconstruct pass).
  - Binding `1` (`STORAGE_IMAGE`): `u_final_output` (`rgba16f`).
  - Push constants (`12` bytes): `uvec2 output_size`, `float sharpness`.
- **Vulkan 1.4 Barriers:**
  1. Upload/transition inputs + `history_*_in` to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` (`VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT`, `VK_ACCESS_2_SHADER_SAMPLED_READ_BIT`); transition `history_*_out` to `VK_IMAGE_LAYOUT_GENERAL` (`VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT`).
  2. Between `ssr_reconstruct.comp` and `ssr_sharpen.comp`, transition `history_color_out` from `VK_IMAGE_LAYOUT_GENERAL` (`VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT`) to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` (`VK_ACCESS_2_SHADER_SAMPLED_READ_BIT`), keeping `history_color_out` unsharpened for next frame's `history_color_in`.
  3. After `ssr_sharpen.comp`, transition `u_final_output` to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` (or `TRANSFER_SRC_OPTIMAL` for readback).

## 4. Measurable CPU/GPU Parity Tolerances & Justification
- **Required Thresholds (`VulkanHarnessConfig`):** `cpu_vs_gpu_psnr_db >= 35.0 dB` and `cpu_vs_gpu_mae <= 0.025` (tone-mapped sRGB `[0, 1]`), with `validation_clean == true`.
- **Justification:** The GPU shader stores temporal history and final output in `VK_FORMAT_R16G16B16A16_SFLOAT` (`RGBA16F`, ~3 decimal digits of mantissa precision) and uses hardware bilinear texture filtering (`VK_FILTER_LINEAR`) for current-color and history resampling, whereas the CPU reference uses FP32 storage and 16-tap Catmull-Rom resampling. Across dynamic scenes and HDR highlights, these precision and filter differences produce `~0.001–0.015` linear deltas (`35–48 dB` PSNR), well above quantization noise while catching any sign/barrier/layout/descriptor bug immediately.

## 5. Ownership & Non-Overlap Boundary
- Gemini owns `include/saige/ssr.hpp`, `src/ssr.cpp`, `shaders/ssr_*.comp`, and `gpu/ssr_vulkan_harness.*`.
- Claude owns `IFrameSink`, Vulkan 1.4 swapchain/command-buffer orchestration, G-buffer motion vector generation, and tone-mapping integration.
