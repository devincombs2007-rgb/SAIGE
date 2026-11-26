# SSR Build 002 — Integration Guide

1. **Include & Link:** Copy `source/include/saige/ssr.hpp`, `source/src/ssr.cpp`, `shaders/ssr_reconstruct.comp`, `shaders/ssr_sharpen.comp`, and `gpu/ssr_vulkan_harness.*` into your engine target using `integration/CMakeLists.snippet.cmake`.
2. **SPIR-V Compilation (Requires Vulkan SDK):**
   ```bash
   glslangValidator -V --target-env vulkan1.4 shaders/ssr_reconstruct.comp -o ssr_reconstruct.spv
   glslangValidator -V --target-env vulkan1.4 shaders/ssr_reconstruct.comp -DSSR_USE_FP16=1 -o ssr_reconstruct_fp16.spv
   glslangValidator -V --target-env vulkan1.4 shaders/ssr_sharpen.comp -o ssr_sharpen.spv
   spirv-val --target-env vulkan1.4 ssr_reconstruct.spv
   spirv-val --target-env vulkan1.4 ssr_sharpen.spv
   ```
3. **Engine Hookup:** Follow `docs/SSR_ENGINE_INTEGRATION_CONTRACT.md` for `IFrameSink` ordering, `pre_exposure`, `camera_cut`, `transparency_mask`, and Vulkan 1.4 `VkImageMemoryBarrier2` transitions.
