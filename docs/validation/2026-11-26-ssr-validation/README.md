# SSR Build 002.1: independent validation evidence (2026-11-26)

Engine commit: see the checkpoint on issue #43 (branch `claude/ssr-validation`). Environment: Linux container, GCC 13.3 and Clang present, Vulkan loader 1.3.275, **llvmpipe** (LLVM 20.1.2) reporting Vulkan 1.4.318, `VK_LAYER_KHRONOS_validation` with synchronization validation, glslang 15.1.0, SPIRV-Tools 2025.1, pinned Vulkan headers 1.4.363. **Software Vulkan only: nothing here is GPU or performance evidence.** The summary of defects and the exact changes are in [`validation/ssr/README.md`](../../../validation/ssr/README.md).

## What ran (all local, Release unless stated; hosted lavapipe lane results are in the checkpoint)
| Check | Result |
| --- | --- |
| Manifest | 24 of 24 files match; patch applied to an empty directory equals the sources; visuals match their manifest hashes |
| C++20 compile, `-Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow` | clean (harness, CPU reference, both test programs) |
| ASan + UBSan | validation suite and Gemini's suite clean (`logs/validation_asan_ubsan.log`) |
| SPIR-V | FP32, FP16 and sharpen binaries compile with glslang and pass `spirv-val --target-env vulkan1.4`; descriptor types, bindings and push-constant offsets (36 and 12 bytes) match the host structures (`ssr_spirv_interface`) |
| Vulkan execution | reconstruction and sharpening dispatched, GPU timestamps valid (llvmpipe), persistent history verified (output with history differs from the same frame after `reset_history`), image readback verified, validation + synchronization validation clean |
| GPU vs CPU | FP32 and FP16 paths, 9-frame sequence (jitter, dynamic resolution, camera cut, exposure x2 and x0.5, transparency and reactive masks): worst 38.99 dB, worst MAE 0.0027, both paths identical to two decimals; tight sequence 44.5 dB (cut and first frames 79.2 dB) |
| Negative tests | empty, short, long, NaN, infinite, all-zero and bright wrong references; invalid dispatch inputs; oversize; bad SPIR-V; missing SPIR-V; injected validation error; layer off; strict device selection; resource churn (create, run, destroy x6, idempotent destroy); resizes |
| Mutation | 14 of 14 shader mutants and 3 of 3 harness mutants caught (the delivered parity gate caught 2 of 14) |
| Gemini's own suite on the corrected harness | passes (4 suites), including the GPU suite that failed its validation check as delivered |
| ctest on this tree | 20 of 20 pass (`logs/ctest_release.log`) |
| Reproduced benchmarks | the delivered CPU numbers reproduce exactly (for example vehicle and fence: SSR 26.38 dB / 0.788 SSIM, TAA-U 25.37 dB / 0.655); CPU times differ with the machine (92 ms here against 64 ms delivered) |

## Measured (640x360 output, Performance 50%, 24 frames per scene, frames 4 to 23; `ssr_gpu_benchmark.json`)
| Scene | PSNR bilinear / bicubic / TAA-U / SSR CPU / **SSR GPU** | SSIM bilinear / TAA-U / SSR CPU / **SSR GPU** | Flicker TAA-U / SSR CPU / **SSR GPU** | GPU vs CPU worst |
| --- | --- | --- | --- | --- |
| vehicle and fence | 23.69 / 23.43 / 24.58 / 25.99 / **25.43** | 0.661 / 0.704 / 0.801 / **0.761** | 0.0133 / 0.0103 / **0.0099** | 39.77 dB |
| foliage and wires | 26.67 / 25.86 / 27.18 / 27.60 / **27.43** | 0.652 / 0.623 / 0.662 / **0.625** | 0.0228 / 0.0224 / **0.0222** | 41.29 dB |
| night HDR | 29.23 / 28.25 / 29.13 / 30.76 / **30.42** | 0.990 / 0.989 / 0.991 / **0.990** | 0.0126 / 0.0063 / **0.0062** | 42.98 dB |
| road and glass | 34.71 / 34.10 / 34.10 / 37.72 / **37.36** | 0.986 / 0.976 / 0.989 / **0.988** | 0.0090 / 0.0042 / **0.0040** | 51.90 dB |
| rapid pan and cut | 23.70 / 23.43 / 24.43 / 25.80 / **25.33** | 0.662 / 0.702 / 0.790 / **0.754** | 0.0774 / 0.0678 / **0.0706** | **34.74 dB (< 35)** |

The "native reference" is the procedural scene's 4x4 supersampled ground truth; there is no native-resolution render of SAIGE content. GPU frame times on llvmpipe (about 15 ms) are not reported as performance.

## What was looked at (by the author; the clip was not played)
* **Detail crops, 1080p output, 1:1 enlarged 3x** (`*_detail_1to1_crop.jpg`, taken where the GPU error against the truth is largest): in `vehicle_fence` the truth is a grid with diagonal hatching; bilinear loses the vertical lines entirely, the TAA-U baseline shows a smeared grid with a stripe artifact at the left edge, the SSR GPU output recovers both grid directions with about half the contrast and broken, stair-stepped diagonals. In `foliage_wires` bilinear breaks the two thin wires into dashes; TAA-U and SSR both keep them continuous but thicker and softer than the truth, and both blur the leaf checker. SSR is clearly better than bilinear on the grid and about equal to TAA-U on the foliage.
* **Contact sheet of the 150-frame clip** (`ssr_gpu_clip_contact_sheet.jpg`, frames 0, 20, 40, 74, 76, 140): the TAA-U baseline shows a visible ghost trail behind the moving striped rectangle (frames 40 and 74); the SSR panel shows none at that size. In the night HDR segment the error map shows a thin crescent on the trailing edge of the moving disc (a small lag), no flicker visible in six frames.
* `ssr_gpu_720p30_diagnostic.mp4` (5 s, 1280x720, 30 fps, H.264): top left truth, top right TAA-U, bottom left SSR (Vulkan compute), bottom right |SSR - truth| x6, a 1:1 centre crop of the 960x540 output. **Not played; only the contact-sheet frames were inspected.**
* The delivered `ssr_1080p_day_comparison.png`: a 2x2 mosaic (truth, bilinear, bicubic, SSR) of an abstract line pattern; in the SSR panel the vertical grid lines of the truth are missing (aliasing to a different pattern).

## Not looked at / not covered
Reflective surfaces, transparent glass and moving vehicles as such: the scenes are procedural patterns. HDR lighting beyond the synthetic bright disc. No real SAIGE frame was reconstructed (SAIGE produces no motion vectors yet). No DLSS or FSR comparison, and none is claimed.
