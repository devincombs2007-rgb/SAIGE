# Changelog — SSR (SAIGE Super Resolution)

## Build 002.1 (2026-11-26)
- **Full Vulkan 1.4 Compute Execution Harness:** Implemented logical device & compute queue creation, device-local images & host-visible staging buffers, descriptor layouts/pools/sets, compute pipeline creation, push constants, `VkImageMemoryBarrier2` layout transitions, ping-pong temporal history, `VkQueryPool` GPU timestamps, `RGBA16F` readback, FP16 (`SSR_USE_FP16`) + FP32 fallback, and CPU-to-GPU parity metrics (`cpu_vs_gpu_psnr_db >= 35.0`, `cpu_vs_gpu_mae <= 0.025`).
- **Genuine FP16 Compute Path:** Updated `shaders/ssr_reconstruct.comp` so `SSR_USE_FP16` executes `float16_t` / `f16vec3` arithmetic for HDR tone compression, YCoCg conversion, clipping, and temporal blending.
- **CPU Execution-Time Regression Fix:** Eliminated the ~20% CPU regression (`75.27 ms -> 63.83 ms`) by precomputing render-resolution tone-compressed YCoCg and sanitized depth once per render pixel (`ren_ycocg_scratch_`, `ren_depth_scratch_`) and using O(1) `std::swap` for ping-pong history.
- **Conventional TAA-U Baseline & Expanded Tests:** Added `TaauBaselineContext` to the 5-scene benchmark suite, added `test_vulkan_gpu_parity_suite` (multi-frame, camera cuts, disocclusion, exposure changes, transparency, dynamic resolution, invalid inputs), and expanded negative/mutation tests (zero-jitter mutation, oversized render dims, null pointers, exposure-jump invariance).

## Build 002 (2026-11-26)
- Added depth-based disocclusion detection, reversible HDR tone compression, `transparency_mask`, separated RCAS sharpening, 5-scene benchmarks, and 1080p/720p30 visuals.

## Build 001 (Baseline)
- Initial C++20 reference SSR implementation (`25.74 dB PSNR / 0.781 SSIM` on 360p Performance).
