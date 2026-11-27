# SSR Build 001 vs Build 002 vs Build 002.1 Comparison

## 1. Algorithmic & GPU Upgrades
- **Build 002 -> 002.1 GPU Harness:** Replaced the probe-only stub in `gpu/ssr_vulkan_harness.cpp` with a complete stateful Vulkan 1.4 compute execution harness (logical device, compute queue, staging buffers, 2D images, descriptor layouts/pools/sets, compute pipelines, push constants, `VkImageMemoryBarrier2` transitions, ping-pong temporal history, `VkQueryPool` GPU timestamps, `RGBA16F` readback, FP16/FP32 SPIR-V selection, and CPU/GPU parity metrics).
- **CPU Performance Regression Investigation & Fix:** Build 002 exhibited a ~20% CPU regression (`75.27 ms` vs Build 001's `62.52 ms` on 360p Performance) because the 3x3 neighborhood loop called `sanitize_rgb` + `hdr_compress` + `rgb_to_ycocg` 9 times per output pixel (`~2.07M` calls/frame) and copied full `history_*` vectors each frame. Build 002.1 precomputes `ren_ycocg_scratch_` and `ren_depth_scratch_` once per render pixel (`57.6K` calls/frame — 36x fewer) and uses O(1) `std::swap` for ping-pong history, recovering CPU time to **`63.55 ms`** (`~16%` faster than Build 002, matching Build 001).

## 2. Quantitative Comparison (360p Performance Mode, 50% Scale)
### Build 001 / 002 / 002.1 Regression Check (`moving_vehicle_and_fence`)
| Implementation | PSNR (dB) | SSIM | Temporal Flicker RMSE | Release CPU (ms) |
| :--- | :--- | :--- | :--- | :--- |
| Bilinear Baseline | `24.7541` | `0.6906` | `0.0472` | — |
| Bicubic (Catmull-Rom) | `24.5295` | `0.7089` | — | — |
| Conventional TAA-U Baseline | `25.3658` | `0.6546` | `0.0142` | — |
| **SSR Build 001** | `25.7367` | `0.7807` | — | `62.52` |
| **SSR Build 002** | `26.3786` | `0.7880` | `0.0129` | `75.27` |
| **SSR Build 002.1** | **`26.3786`** | **`0.7880`** | **`0.0129`** | **`63.83`** |

### Expanded Multi-Scene Suite (vs Conventional TAA-U, Bilinear, Bicubic)
| Scene | Bilinear PSNR / SSIM | TAA-U PSNR / SSIM / Flicker | SSR 0.2.1 PSNR / SSIM / Flicker |
| :--- | :--- | :--- | :--- |
| `moving_vehicle_and_fence` | `24.7541 / 0.6906` | `25.3658 / 0.6546 / 0.0142` | **`26.3786 / 0.7880 / 0.0129`** |
| `fine_foliage_and_power_lines` | `25.8764 / 0.5481` | `27.7997 / 0.6070 / 0.0230` | **`27.6711 / 0.6475 / 0.0231`** |
| `nighttime_neon_hdr` | `28.7732 / 0.9880` | `29.5354 / 0.9890 / 0.0143` | **`30.8531 / 0.9900 / 0.0083`** |
| `reflective_road_and_glass` | `34.8128 / 0.9834` | `33.7936 / 0.9737 / 0.0095` | **`37.7366 / 0.9881 / 0.0048`** |
| `rapid_camera_pan_and_cut` | `24.7413 / 0.6914` | `25.2792 / 0.6569 / 0.0778` | **`26.5703 / 0.7892 / 0.0674`** |
