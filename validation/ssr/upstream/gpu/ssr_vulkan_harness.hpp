#pragma once

#include "saige/ssr.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace saige::ssr::gpu {

struct VulkanHarnessConfig {
  bool enable_validation_layers = true;
  bool prefer_software_vulkan = true;  // Allows Lavapipe correctness testing
  bool prefer_fp16 = true;
  std::string reconstruct_spv_path = "shaders/ssr_reconstruct.spv";
  std::string reconstruct_fp16_spv_path = "shaders/ssr_reconstruct_fp16.spv";
  std::string sharpen_spv_path = "shaders/ssr_sharpen.spv";
  float sharpness = 0.5f;
  float disocclusion_depth_threshold = 0.05f;
  // Parity tolerances: GPU uses RGBA16F history/output + bilinear texture
  // sampling, whereas CPU reference uses FP32 + Catmull-Rom; >=35 dB PSNR and
  // <=0.025 MAE in tone-mapped sRGB [0,1] ensure algorithmic equivalence.
  double min_parity_psnr_db = 35.0;
  double max_parity_mae = 0.025;
};

struct VulkanParityReport {
  bool executed = false;
  bool validation_clean = false;
  bool fp16_active = false;
  bool parity_passed = false;
  std::string device_name;
  bool is_software_device = false;
  double reconstruct_gpu_ms = 0.0;
  double sharpen_gpu_ms = 0.0;
  double gpu_time_ms = 0.0;
  double cpu_vs_gpu_psnr_db = 0.0;
  double cpu_vs_gpu_mae = 0.0;
  double cpu_vs_gpu_max_abs_err = 0.0;
  std::string status_message;
};

// Stateful Vulkan 1.4 compute execution harness supporting persistent
// multi-frame ping-pong history, timestamps, readback, and CPU/GPU parity.
class SsrVulkanHarness {
 public:
  SsrVulkanHarness() = default;
  ~SsrVulkanHarness();

  SsrVulkanHarness(const SsrVulkanHarness&) = delete;
  SsrVulkanHarness& operator=(const SsrVulkanHarness&) = delete;

  [[nodiscard]] bool initialize(const VulkanHarnessConfig& cfg,
                                std::string& out_status);
  void reset_history() noexcept;
  void destroy() noexcept;

  [[nodiscard]] VulkanParityReport execute_frame(
      const SsrDispatchInputs& inputs,
      const std::vector<ColorRgb>& cpu_reference_output,
      std::vector<ColorRgb>& out_gpu_color);

  // Single-frame convenience wrapper
  [[nodiscard]] static VulkanParityReport run_parity_test(
      const VulkanHarnessConfig& cfg,
      const SsrDispatchInputs& inputs,
      const std::vector<ColorRgb>& cpu_reference_output);

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace saige::ssr::gpu
