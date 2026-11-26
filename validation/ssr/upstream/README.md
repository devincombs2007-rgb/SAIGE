# SAIGE Super Resolution (SSR) — Build 002.1

**Target Engine:** [SAIGE (Sensational Artificial Intelligent Game Engine)](https://github.com/devincombs2007-rgb/SAIGE)  
**Status:** CPU Reconstruction, TAA-U Baseline, & 4 Test Suites Validated | Vulkan SPIR-V & GPU Execution **UNVERIFIED on Delivery VM** (complete implementation delivered; Vulkan SDK / `glslangValidator` / `vulkan.h` absent on VM — see `HANDOFF.md` for CI commands)

## Quick Build & Test (C++20)
```bash
# Release (-O2 -DNDEBUG)
g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -Wconversion \
  -Isource/include -Igpu \
  source/src/ssr.cpp gpu/ssr_vulkan_harness.cpp tests/ssr_test.cpp \
  -o ssr21_rel_test
./ssr21_rel_test

# ASan + UBSan
g++ -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Werror -Wconversion \
  -fsanitize=address,undefined -Isource/include -Igpu \
  source/src/ssr.cpp gpu/ssr_vulkan_harness.cpp tests/ssr_test.cpp \
  -o ssr21_asan_test
./ssr21_asan_test
```

## Key Results (360p Performance Mode, 50% Scale)
- `moving_vehicle_and_fence`: **26.38 dB / 0.788 SSIM / 0.0129 Flicker** (`63.83 ms` Release CPU vs Build 002's `75.27 ms`; TAA-U: `25.37 dB / 0.655 SSIM`).
- `fine_foliage_and_power_lines`: **27.67 dB / 0.647 SSIM / 0.0231 Flicker** (TAA-U: `27.80 dB / 0.607 SSIM`).
- `nighttime_neon_hdr`: **30.85 dB / 0.990 SSIM / 0.0083 Flicker** (TAA-U: `29.54 dB / 0.989 SSIM / 0.0143 Flicker`).
- `reflective_road_and_glass`: **37.74 dB / 0.988 SSIM / 0.0048 Flicker** (TAA-U: `33.79 dB / 0.974 SSIM / 0.0095 Flicker`).
- `rapid_camera_pan_and_cut`: **26.57 dB / 0.789 SSIM / 0.0674 Flicker** (TAA-U: `25.28 dB / 0.657 SSIM / 0.0778 Flicker`).
