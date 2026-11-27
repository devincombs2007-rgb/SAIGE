# Handoff — SSR Build 002.1

- **Delivered Package:** `SAIGE_Gemini_SSR_Build_002_1.zip`
- **Maintainer Boundary:** Gemini owns SSR (`source/`, `shaders/`, `gpu/`, `tests/`); Claude owns SAIGE `IFrameSink` / Vulkan 1.4 swapchain orchestration (`docs/SSR_ENGINE_INTEGRATION_CONTRACT.md`).

## Reproducible Vulkan Validation Commands for Claude (SAIGE CI Environment)
Run these commands from the extracted package root on a runner with the Vulkan SDK (`glslangValidator`, `spirv-val`, `libvulkan-dev`, and `VK_LAYER_KHRONOS_validation` / Lavapipe or GPU):
```bash
# 1. Compile and validate SPIR-V shaders (FP32, FP16, and RCAS sharpen)
glslangValidator -V --target-env vulkan1.4 shaders/ssr_reconstruct.comp -o shaders/ssr_reconstruct.spv
glslangValidator -V --target-env vulkan1.4 shaders/ssr_reconstruct.comp -DSSR_USE_FP16=1 -o shaders/ssr_reconstruct_fp16.spv
glslangValidator -V --target-env vulkan1.4 shaders/ssr_sharpen.comp -o shaders/ssr_sharpen.spv
spirv-val --target-env vulkan1.4 shaders/ssr_reconstruct.spv
spirv-val --target-env vulkan1.4 shaders/ssr_reconstruct_fp16.spv
spirv-val --target-env vulkan1.4 shaders/ssr_sharpen.spv

# 2. Build & run full C++20 + Vulkan 1.4 CPU/GPU parity suite (Release & ASan+UBSan)
g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -Wconversion \
  -DSAIGE_SSR_ENABLE_VULKAN=1 -Isource/include -Igpu \
  source/src/ssr.cpp gpu/ssr_vulkan_harness.cpp tests/ssr_test.cpp \
  -lvulkan -o ssr21_vulkan_rel
./ssr21_vulkan_rel

g++ -std=c++20 -O2 -g -Wall -Wextra -Wpedantic -Werror -Wconversion \
  -fsanitize=address,undefined -DSAIGE_SSR_ENABLE_VULKAN=1 -Isource/include -Igpu \
  source/src/ssr.cpp gpu/ssr_vulkan_harness.cpp tests/ssr_test.cpp \
  -lvulkan -o ssr21_vulkan_asan
./ssr21_vulkan_asan
```

## Unverified Items on Delivery VM
- Live SPIR-V compilation (`glslangValidator` / `spirv-val`) and Vulkan execution (`-DSAIGE_SSR_ENABLE_VULKAN=1 -lvulkan`) are **UNVERIFIED** on the delivery VM due to missing Vulkan SDK headers and shader compiler tools.
- **Preserved Frozen Divisions:** World Simulation Build 010 and World Editor Build 011 remain untouched.
