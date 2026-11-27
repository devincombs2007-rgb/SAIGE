#pragma once

#include <cstdint>
#include <vector>

namespace saige::ssr {

inline constexpr std::uint32_t kSsrContractVersion = 2;

enum class SsrQualityMode : std::uint8_t {
  NativeAA = 0,          // 100% per axis
  UltraQuality = 1,      // 77% per axis
  Quality = 2,           // 67% per axis
  Balanced = 3,          // 58% per axis
  Performance = 4,       // 50% per axis
  UltraPerformance = 5,  // 33% per axis
};

struct ColorRgb {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
};

struct Vec2f {
  float x = 0.0f;
  float y = 0.0f;
};

struct SsrResolution {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

struct SsrInitConfig {
  std::uint32_t max_output_width = 3840;
  std::uint32_t max_output_height = 2160;
  SsrQualityMode quality_mode = SsrQualityMode::Quality;
  bool enable_sharpening = true;
  float sharpness = 0.25f;  // [0.0, 1.0]
  float disocclusion_depth_threshold = 0.05f;
};

// Versioned per-frame dispatch contract between Claude's Vulkan RHI and SSR.
// Coordinate & Input Conventions:
// - Right-handed Z-up world; screen-space UV/pixel origin at top-left (0, 0).
// - motion_vectors_px: (prev_render_px - curr_render_px), i.e., adding MV
//   to current pixel coordinate yields previous frame's sample coordinate.
// - jitter_offset_px: subpixel camera jitter in [-0.5, 0.5] render pixels.
// - depth: normalized [0, 1]; reversed_z = true means 1.0 = near, 0.0 = far.
// - pre_exposure: linear multiplier applied to scene color prior to SSR.
struct SsrDispatchInputs {
  std::uint32_t render_width = 0;
  std::uint32_t render_height = 0;
  std::uint32_t output_width = 0;
  std::uint32_t output_height = 0;

  const ColorRgb* color_hdr = nullptr;       // size = render_width * render_height
  const float* depth = nullptr;              // size = render_width * render_height
  const Vec2f* motion_vectors_px = nullptr;  // size = render_width * render_height
  const float* reactive_mask = nullptr;      // optional [0, 1]
  const float* transparency_mask = nullptr;  // optional [0, 1]

  Vec2f jitter_offset_px{};
  float pre_exposure = 1.0f;
  bool camera_cut = false;
  bool reversed_z = true;
};

struct SsrDispatchMetrics {
  bool valid = false;
  std::uint32_t history_reset_pixels = 0;
  std::uint32_t disoccluded_pixels = 0;
  std::uint32_t clamped_pixels = 0;
  std::uint32_t sanitized_invalid_inputs = 0;
  double cpu_time_ms = 0.0;
};

class SsrContext {
 public:
  explicit SsrContext(const SsrInitConfig& config = {});

  [[nodiscard]] static float scale_for_mode(SsrQualityMode mode) noexcept;
  [[nodiscard]] static SsrResolution compute_render_resolution(
      std::uint32_t output_width, std::uint32_t output_height,
      SsrQualityMode mode) noexcept;
  [[nodiscard]] static Vec2f halton_jitter_px(std::uint32_t frame_index,
                                              std::uint32_t phase_count = 16) noexcept;

  void reset_history() noexcept;
  SsrDispatchMetrics reconstruct(const SsrDispatchInputs& inputs,
                                 std::vector<ColorRgb>& out_color);

  [[nodiscard]] const std::vector<ColorRgb>& history() const noexcept {
    return history_color_;
  }
  [[nodiscard]] std::size_t estimated_memory_bytes() const noexcept;

 private:
  SsrInitConfig config_{};
  std::uint32_t history_width_ = 0;
  std::uint32_t history_height_ = 0;
  float prev_exposure_ = 1.0f;
  bool has_history_ = false;
  std::vector<ColorRgb> history_color_;
  std::vector<float> history_depth_;
  std::vector<float> history_confidence_;
  std::vector<ColorRgb> accum_scratch_;
  std::vector<float> depth_scratch_;
  std::vector<float> conf_scratch_;
  std::vector<ColorRgb> ren_ycocg_scratch_;
  std::vector<float> ren_depth_scratch_;
};

void upscale_bilinear(const ColorRgb* src, std::uint32_t src_w, std::uint32_t src_h,
                      std::uint32_t dst_w, std::uint32_t dst_h,
                      std::vector<ColorRgb>& out);
void upscale_bicubic(const ColorRgb* src, std::uint32_t src_w, std::uint32_t src_h,
                     std::uint32_t dst_w, std::uint32_t dst_h,
                     std::vector<ColorRgb>& out);

}  // namespace saige::ssr
