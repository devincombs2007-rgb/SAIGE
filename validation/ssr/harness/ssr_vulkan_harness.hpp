#pragma once

#include "saige/ssr.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace saige::ssr::gpu {

struct VulkanHarnessConfig {
  bool enable_validation_layers = true;
  // VALIDATION CHANGE (SAIGE): synchronization validation is requested through VK_EXT_validation_features when the layer
  // offers it; the report says whether it was actually active.
  bool enable_sync_validation = true;
  bool prefer_software_vulkan = true;  // Allows Lavapipe correctness testing
  // VALIDATION CHANGE (SAIGE): when true, a device of the preferred kind must exist; the harness no longer falls back to
  // physical device 0 silently (a software-Vulkan result could otherwise be reported as a hardware result, or the reverse).
  bool strict_device_selection = false;
  bool prefer_fp16 = true;
  // VALIDATION CHANGE (SAIGE): force the FP32 reconstruct binary even when the device offers shaderFloat16, so both paths
  // can be run on one device. (Mutated shaders are run by pointing the *_spv_path fields at the mutant binaries.)
  bool force_fp32 = false;
  // VALIDATION CHANGE (SAIGE): test hook. Makes execute_frame issue one deliberately invalid call (an undefined VkFormat enum) so a test can
  // prove the validation layer's messages reach the report; never set outside tests.
  bool inject_validation_error = false;
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
  // VALIDATION CHANGE (SAIGE): validation_clean is true only when the validation layer was active and reported nothing at
  // warning level or above; with the layer off it is false (it used to default to true).
  bool validation_clean = false;
  bool validation_active = false;
  bool sync_validation_active = false;
  std::uint32_t validation_message_count = 0;
  std::string first_validation_message;
  bool fp16_active = false;
  // VALIDATION CHANGE (SAIGE): parity_passed is true only when a valid reference of exactly the output size was supplied
  // (non-empty, finite) AND the metrics are finite and inside the thresholds. A missing, empty, mis-sized or non-finite
  // reference is parity_evaluated == false and parity_passed == false (it used to count as success when empty).
  bool parity_evaluated = false;
  bool reference_valid = false;
  bool parity_passed = false;
  bool timestamps_valid = false;
  std::uint32_t api_version = 0;
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
