#include "saige/ssr.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace saige::ssr {

namespace {

[[nodiscard]] float clampf(float v, float lo, float hi) noexcept {
  if (!std::isfinite(v) || v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

[[nodiscard]] ColorRgb sanitize_rgb(ColorRgb c,
                                    std::uint32_t& invalid_count) noexcept {
  if (!std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b)) {
    ++invalid_count;
    return {0.0f, 0.0f, 0.0f};
  }
  return {std::max(0.0f, c.r), std::max(0.0f, c.g), std::max(0.0f, c.b)};
}

[[nodiscard]] float luminance(ColorRgb c) noexcept {
  return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
}

// Reversible Karis/tone-compression for HDR highlight stability
[[nodiscard]] ColorRgb hdr_compress(ColorRgb c) noexcept {
  const float inv = 1.0f / (1.0f + luminance(c));
  return {c.r * inv, c.g * inv, c.b * inv};
}

[[nodiscard]] ColorRgb hdr_decompress(ColorRgb c) noexcept {
  const float l = std::min(0.999f, luminance(c));
  const float inv = 1.0f / (1.0f - l);
  return {c.r * inv, c.g * inv, c.b * inv};
}

[[nodiscard]] ColorRgb rgb_to_ycocg(ColorRgb c) noexcept {
  const float co = c.r - c.b;
  const float tmp = c.b + co * 0.5f;
  const float cg = c.g - tmp;
  const float y = tmp + cg * 0.5f;
  return {y, co, cg};
}

[[nodiscard]] ColorRgb ycocg_to_rgb(ColorRgb c) noexcept {
  const float tmp = c.r - c.b * 0.5f;
  const float g = c.b + tmp;
  const float b = tmp - c.g * 0.5f;
  const float r = b + c.g;
  return {std::max(0.0f, r), std::max(0.0f, g), std::max(0.0f, b)};
}

[[nodiscard]] float halton(std::uint32_t index, std::uint32_t base) noexcept {
  float f = 1.0f;
  float r = 0.0f;
  while (index > 0) {
    f /= static_cast<float>(base);
    r += f * static_cast<float>(index % base);
    index /= base;
  }
  return r;
}

[[nodiscard]] ColorRgb sample_clamped(const ColorRgb* buf, std::uint32_t w,
                                      std::uint32_t h, int x, int y) noexcept {
  const int cx = std::clamp(x, 0, static_cast<int>(w) - 1);
  const int cy = std::clamp(y, 0, static_cast<int>(h) - 1);
  return buf[static_cast<std::size_t>(cy) * w + static_cast<std::size_t>(cx)];
}

[[nodiscard]] float sample_depth_bilinear(const float* buf, std::uint32_t w,
                                          std::uint32_t h, float fx,
                                          float fy) noexcept {
  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const float tx = clampf(fx - static_cast<float>(x0), 0.0f, 1.0f);
  const float ty = clampf(fy - static_cast<float>(y0), 0.0f, 1.0f);
  auto at = [&](int x, int y) {
    const int cx = std::clamp(x, 0, static_cast<int>(w) - 1);
    const int cy = std::clamp(y, 0, static_cast<int>(h) - 1);
    return buf[static_cast<std::size_t>(cy) * w + static_cast<std::size_t>(cx)];
  };
  const float a = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * tx;
  const float b = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * tx;
  return a + (b - a) * ty;
}

[[nodiscard]] ColorRgb sample_bilinear(const ColorRgb* buf, std::uint32_t w,
                                       std::uint32_t h, float fx,
                                       float fy) noexcept {
  const int x0 = static_cast<int>(std::floor(fx));
  const int y0 = static_cast<int>(std::floor(fy));
  const float tx = clampf(fx - static_cast<float>(x0), 0.0f, 1.0f);
  const float ty = clampf(fy - static_cast<float>(y0), 0.0f, 1.0f);

  const ColorRgb c00 = sample_clamped(buf, w, h, x0, y0);
  const ColorRgb c10 = sample_clamped(buf, w, h, x0 + 1, y0);
  const ColorRgb c01 = sample_clamped(buf, w, h, x0, y0 + 1);
  const ColorRgb c11 = sample_clamped(buf, w, h, x0 + 1, y0 + 1);

  auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
  return {
      lerp(lerp(c00.r, c10.r, tx), lerp(c01.r, c11.r, tx), ty),
      lerp(lerp(c00.g, c10.g, tx), lerp(c01.g, c11.g, tx), ty),
      lerp(lerp(c00.b, c10.b, tx), lerp(c01.b, c11.b, tx), ty),
  };
}

[[nodiscard]] float catmull_rom_weight(float x) noexcept {
  const float ax = std::abs(x);
  if (ax < 1.0f) return (1.5f * ax - 2.5f) * ax * ax + 1.0f;
  if (ax < 2.0f) return ((-0.5f * ax + 2.5f) * ax - 4.0f) * ax + 2.0f;
  return 0.0f;
}

[[nodiscard]] ColorRgb sample_catmull_rom(const ColorRgb* buf, std::uint32_t w,
                                          std::uint32_t h, float fx,
                                          float fy) noexcept {
  const int ix = static_cast<int>(std::floor(fx));
  const int iy = static_cast<int>(std::floor(fy));
  const float dx = fx - static_cast<float>(ix);
  const float dy = fy - static_cast<float>(iy);

  ColorRgb sum{0.0f, 0.0f, 0.0f};
  float wsum = 0.0f;
  for (int oy = -1; oy <= 2; ++oy) {
    const float wy = catmull_rom_weight(static_cast<float>(oy) - dy);
    for (int ox = -1; ox <= 2; ++ox) {
      const float wx = catmull_rom_weight(static_cast<float>(ox) - dx);
      const float wt = wx * wy;
      const ColorRgb c = sample_clamped(buf, w, h, ix + ox, iy + oy);
      sum.r += c.r * wt;
      sum.g += c.g * wt;
      sum.b += c.b * wt;
      wsum += wt;
    }
  }
  if (std::abs(wsum) > 1e-5f) {
    const float inv = 1.0f / wsum;
    sum.r = std::max(0.0f, sum.r * inv);
    sum.g = std::max(0.0f, sum.g * inv);
    sum.b = std::max(0.0f, sum.b * inv);
  }
  return sum;
}

}  // namespace

SsrContext::SsrContext(const SsrInitConfig& config) : config_(config) {}

float SsrContext::scale_for_mode(SsrQualityMode mode) noexcept {
  switch (mode) {
    case SsrQualityMode::NativeAA:
      return 1.00f;
    case SsrQualityMode::UltraQuality:
      return 0.77f;
    case SsrQualityMode::Quality:
      return 0.67f;
    case SsrQualityMode::Balanced:
      return 0.58f;
    case SsrQualityMode::Performance:
      return 0.50f;
    case SsrQualityMode::UltraPerformance:
      return 0.33f;
  }
  return 0.67f;
}

SsrResolution SsrContext::compute_render_resolution(
    std::uint32_t output_width, std::uint32_t output_height,
    SsrQualityMode mode) noexcept {
  const float s = scale_for_mode(mode);
  const auto rw = std::max<std::uint32_t>(
      1u, static_cast<std::uint32_t>(
              std::lround(static_cast<float>(output_width) * s)));
  const auto rh = std::max<std::uint32_t>(
      1u, static_cast<std::uint32_t>(
              std::lround(static_cast<float>(output_height) * s)));
  return {rw, rh};
}

Vec2f SsrContext::halton_jitter_px(std::uint32_t frame_index,
                                   std::uint32_t phase_count) noexcept {
  const std::uint32_t p = std::max<std::uint32_t>(1u, phase_count);
  const std::uint32_t idx = (frame_index % p) + 1u;
  return {halton(idx, 2u) - 0.5f, halton(idx, 3u) - 0.5f};
}

void SsrContext::reset_history() noexcept {
  has_history_ = false;
  history_width_ = 0;
  history_height_ = 0;
  history_color_.clear();
  history_depth_.clear();
  history_confidence_.clear();
}

std::size_t SsrContext::estimated_memory_bytes() const noexcept {
  return (history_color_.capacity() + accum_scratch_.capacity() +
          ren_ycocg_scratch_.capacity()) *
             sizeof(ColorRgb) +
         (history_depth_.capacity() + history_confidence_.capacity() +
          depth_scratch_.capacity() + conf_scratch_.capacity() +
          ren_depth_scratch_.capacity()) *
             sizeof(float);
}

SsrDispatchMetrics SsrContext::reconstruct(const SsrDispatchInputs& inputs,
                                           std::vector<ColorRgb>& out_color) {
  const auto t0 = std::chrono::steady_clock::now();
  SsrDispatchMetrics metrics{};

  if (inputs.render_width == 0 || inputs.render_height == 0 ||
      inputs.output_width == 0 || inputs.output_height == 0 ||
      inputs.render_width > inputs.output_width ||
      inputs.render_height > inputs.output_height ||
      inputs.color_hdr == nullptr || inputs.depth == nullptr ||
      inputs.motion_vectors_px == nullptr) {
    return metrics;
  }

  const std::size_t out_count =
      static_cast<std::size_t>(inputs.output_width) * inputs.output_height;
  accum_scratch_.resize(out_count);
  depth_scratch_.resize(out_count);
  conf_scratch_.resize(out_count);
  out_color.resize(out_count);

  const bool resolution_changed = (history_width_ != inputs.output_width ||
                                   history_height_ != inputs.output_height);
  const bool reset_all = !has_history_ || inputs.camera_cut || resolution_changed;
  if (reset_all) {
    history_color_.assign(out_count, ColorRgb{0.0f, 0.0f, 0.0f});
    history_depth_.assign(out_count, inputs.reversed_z ? 0.0f : 1.0f);
    history_confidence_.assign(out_count, 0.0f);
    history_width_ = inputs.output_width;
    history_height_ = inputs.output_height;
  }

  const float safe_exp =
      (std::isfinite(inputs.pre_exposure) && inputs.pre_exposure > 1e-4f)
          ? inputs.pre_exposure
          : 1.0f;
  const float prev_exp =
      (std::isfinite(prev_exposure_) && prev_exposure_ > 1e-4f)
          ? prev_exposure_
          : 1.0f;
  const float exp_ratio = clampf(safe_exp / prev_exp, 0.01f, 100.0f);

  const float scale_x = static_cast<float>(inputs.render_width) /
                        static_cast<float>(inputs.output_width);
  const float scale_y = static_cast<float>(inputs.render_height) /
                        static_cast<float>(inputs.output_height);
  const float inv_scale_x = 1.0f / scale_x;
  const float inv_scale_y = 1.0f / scale_y;

  const float jitter_x =
      std::isfinite(inputs.jitter_offset_px.x) ? inputs.jitter_offset_px.x : 0.0f;
  const float jitter_y =
      std::isfinite(inputs.jitter_offset_px.y) ? inputs.jitter_offset_px.y : 0.0f;

  // Precompute tone-compressed YCoCg and sanitized depth once per render pixel
  const std::size_t ren_count =
      static_cast<std::size_t>(inputs.render_width) * inputs.render_height;
  ren_ycocg_scratch_.resize(ren_count);
  ren_depth_scratch_.resize(ren_count);
  for (std::size_t i = 0; i < ren_count; ++i) {
    const ColorRgb c_rgb =
        sanitize_rgb(inputs.color_hdr[i], metrics.sanitized_invalid_inputs);
    ren_ycocg_scratch_[i] = rgb_to_ycocg(hdr_compress(c_rgb));
    float d = inputs.depth[i];
    if (!std::isfinite(d)) {
      ++metrics.sanitized_invalid_inputs;
      d = inputs.reversed_z ? 0.0f : 1.0f;
    }
    ren_depth_scratch_[i] = d;
  }

  for (std::uint32_t oy = 0; oy < inputs.output_height; ++oy) {
    for (std::uint32_t ox = 0; ox < inputs.output_width; ++ox) {
      const float rx = (static_cast<float>(ox) + 0.5f) * scale_x - 0.5f - jitter_x;
      const float ry = (static_cast<float>(oy) + 0.5f) * scale_y - 0.5f - jitter_y;
      const int ix = std::clamp(static_cast<int>(std::lround(rx)), 0,
                                static_cast<int>(inputs.render_width) - 1);
      const int iy = std::clamp(static_cast<int>(std::lround(ry)), 0,
                                static_cast<int>(inputs.render_height) - 1);

      // 1. 3x3 Closest-depth motion vector dilation + tone-compressed YCoCg stats
      int best_dx = ix;
      int best_dy = iy;
      float best_depth = inputs.reversed_z ? -1.0f : 2.0f;
      ColorRgb ycocg_sum{0.0f, 0.0f, 0.0f};
      ColorRgb ycocg_sq_sum{0.0f, 0.0f, 0.0f};
      ColorRgb ycocg_min{1e9f, 1e9f, 1e9f};
      ColorRgb ycocg_max{-1e9f, -1e9f, -1e9f};

      for (int ky = -1; ky <= 1; ++ky) {
        const int sy =
            std::clamp(iy + ky, 0, static_cast<int>(inputs.render_height) - 1);
        for (int kx = -1; kx <= 1; ++kx) {
          const int sx =
              std::clamp(ix + kx, 0, static_cast<int>(inputs.render_width) - 1);
          const std::size_t r_idx =
              static_cast<std::size_t>(sy) * inputs.render_width +
              static_cast<std::size_t>(sx);

          const ColorRgb c_yc = ren_ycocg_scratch_[r_idx];
          ycocg_sum.r += c_yc.r;
          ycocg_sum.g += c_yc.g;
          ycocg_sum.b += c_yc.b;
          ycocg_sq_sum.r += c_yc.r * c_yc.r;
          ycocg_sq_sum.g += c_yc.g * c_yc.g;
          ycocg_sq_sum.b += c_yc.b * c_yc.b;
          ycocg_min.r = std::min(ycocg_min.r, c_yc.r);
          ycocg_min.g = std::min(ycocg_min.g, c_yc.g);
          ycocg_min.b = std::min(ycocg_min.b, c_yc.b);
          ycocg_max.r = std::max(ycocg_max.r, c_yc.r);
          ycocg_max.g = std::max(ycocg_max.g, c_yc.g);
          ycocg_max.b = std::max(ycocg_max.b, c_yc.b);

          const float d = ren_depth_scratch_[r_idx];
          const bool closer = inputs.reversed_z ? (d > best_depth) : (d < best_depth);
          if (closer) {
            best_depth = d;
            best_dx = sx;
            best_dy = sy;
          }
        }
      }

      const std::size_t best_r_idx =
          static_cast<std::size_t>(best_dy) * inputs.render_width +
          static_cast<std::size_t>(best_dx);
      Vec2f mv = inputs.motion_vectors_px[best_r_idx];
      if (!std::isfinite(mv.x) || !std::isfinite(mv.y)) {
        ++metrics.sanitized_invalid_inputs;
        mv = {0.0f, 0.0f};
      }
      const float mv_out_x = mv.x * inv_scale_x;
      const float mv_out_y = mv.y * inv_scale_y;

      const std::size_t c_r_idx =
          static_cast<std::size_t>(iy) * inputs.render_width +
          static_cast<std::size_t>(ix);
      float center_depth = inputs.depth[c_r_idx];
      if (!std::isfinite(center_depth)) {
        center_depth = inputs.reversed_z ? 0.0f : 1.0f;
      }

      const ColorRgb curr_rgb = sample_catmull_rom(
          inputs.color_hdr, inputs.render_width, inputs.render_height, rx, ry);
      const ColorRgb curr_safe =
          sanitize_rgb(curr_rgb, metrics.sanitized_invalid_inputs);

      const std::size_t o_idx =
          static_cast<std::size_t>(oy) * inputs.output_width + ox;
      depth_scratch_[o_idx] = center_depth;

      if (reset_all) {
        accum_scratch_[o_idx] = curr_safe;
        conf_scratch_[o_idx] = 0.25f;
        ++metrics.history_reset_pixels;
        continue;
      }

      const float hist_x = static_cast<float>(ox) + mv_out_x;
      const float hist_y = static_cast<float>(oy) + mv_out_y;
      const bool out_of_bounds =
          (hist_x < 0.0f || hist_y < 0.0f ||
           hist_x >= static_cast<float>(inputs.output_width) ||
           hist_y >= static_cast<float>(inputs.output_height));

      if (out_of_bounds) {
        accum_scratch_[o_idx] = curr_safe;
        conf_scratch_[o_idx] = 0.25f;
        ++metrics.history_reset_pixels;
        continue;
      }

      // 2. Depth-based disocclusion detection
      const float prev_depth = sample_depth_bilinear(
          history_depth_.data(), inputs.output_width, inputs.output_height,
          hist_x, hist_y);
      const float depth_diff = std::abs(center_depth - prev_depth);
      const bool disoccluded =
          depth_diff > config_.disocclusion_depth_threshold;
      if (disoccluded) {
        ++metrics.disoccluded_pixels;
      }

      ColorRgb hist_rgb = sample_catmull_rom(
          history_color_.data(), inputs.output_width, inputs.output_height,
          hist_x, hist_y);
      hist_rgb.r *= exp_ratio;
      hist_rgb.g *= exp_ratio;
      hist_rgb.b *= exp_ratio;

      // 3. Variance-guided YCoCg neighborhood clipping in tone-compressed HDR space
      constexpr float kInv9 = 1.0f / 9.0f;
      const ColorRgb mean{ycocg_sum.r * kInv9, ycocg_sum.g * kInv9,
                          ycocg_sum.b * kInv9};
      const ColorRgb std_dev{
          std::sqrt(std::max(0.0f, ycocg_sq_sum.r * kInv9 - mean.r * mean.r)),
          std::sqrt(std::max(0.0f, ycocg_sq_sum.g * kInv9 - mean.g * mean.g)),
          std::sqrt(std::max(0.0f, ycocg_sq_sum.b * kInv9 - mean.b * mean.b)),
      };
      const float mv_mag = std::sqrt(mv_out_x * mv_out_x + mv_out_y * mv_out_y);
      // Adaptive gamma: slightly wider on static high-contrast thin features, tighter in motion
      const float gamma =
          clampf(1.45f - std::min(0.45f, mv_mag * 0.04f), 1.00f, 1.50f);

      const ColorRgb clip_min{
          std::max(ycocg_min.r, mean.r - gamma * std_dev.r),
          std::max(ycocg_min.g, mean.g - gamma * std_dev.g),
          std::max(ycocg_min.b, mean.b - gamma * std_dev.b),
      };
      const ColorRgb clip_max{
          std::min(ycocg_max.r, mean.r + gamma * std_dev.r),
          std::min(ycocg_max.g, mean.g + gamma * std_dev.g),
          std::min(ycocg_max.b, mean.b + gamma * std_dev.b),
      };

      const ColorRgb hist_comp_yc = rgb_to_ycocg(hdr_compress(hist_rgb));
      const ColorRgb clipped_yc{
          std::clamp(hist_comp_yc.r, clip_min.r, clip_max.r),
          std::clamp(hist_comp_yc.g, clip_min.g, clip_max.g),
          std::clamp(hist_comp_yc.b, clip_min.b, clip_max.b),
      };
      const bool was_clamped =
          (clipped_yc.r != hist_comp_yc.r || clipped_yc.g != hist_comp_yc.g ||
           clipped_yc.b != hist_comp_yc.b);
      if (was_clamped) {
        ++metrics.clamped_pixels;
      }

      // 4. Confidence-based & mask-aware temporal accumulation in compressed HDR space
      const float prev_conf = sample_depth_bilinear(
          history_confidence_.data(), inputs.output_width, inputs.output_height,
          hist_x, hist_y);
      float new_conf = disoccluded ? 0.15f : std::min(1.0f, prev_conf + 0.20f);
      if (was_clamped && !disoccluded) new_conf = std::max(0.25f, new_conf * 0.85f);

      float reactive =
          inputs.reactive_mask ? clampf(inputs.reactive_mask[c_r_idx], 0.0f, 1.0f)
                               : 0.0f;
      const float transp =
          inputs.transparency_mask
              ? clampf(inputs.transparency_mask[c_r_idx], 0.0f, 1.0f)
              : 0.0f;
      reactive = std::max(reactive, transp * 0.7f);

      const float base_alpha = 0.10f + (1.0f - new_conf) * 0.25f +
                               clampf(mv_mag * 0.015f, 0.0f, 0.15f);
      float alpha = std::clamp(base_alpha + reactive * (1.0f - base_alpha),
                               0.10f, 1.0f);
      if (disoccluded) {
        alpha = std::max(alpha, 0.85f);
      }

      const ColorRgb hist_comp_rgb = ycocg_to_rgb(clipped_yc);
      const ColorRgb curr_comp_rgb = hdr_compress(curr_safe);
      const ColorRgb blended_comp{
          hist_comp_rgb.r * (1.0f - alpha) + curr_comp_rgb.r * alpha,
          hist_comp_rgb.g * (1.0f - alpha) + curr_comp_rgb.g * alpha,
          hist_comp_rgb.b * (1.0f - alpha) + curr_comp_rgb.b * alpha,
      };
      accum_scratch_[o_idx] = hdr_decompress(blended_comp);
      conf_scratch_[o_idx] = new_conf;
    }
  }

  // Persist unsharpened temporal history (O(1) swap)
  std::swap(history_color_, accum_scratch_);
  std::swap(history_depth_, depth_scratch_);
  std::swap(history_confidence_, conf_scratch_);
  prev_exposure_ = safe_exp;
  has_history_ = true;

  // 5. Separated post-history edge-adaptive RCAS-style sharpening with anti-ringing
  if (config_.enable_sharpening && config_.sharpness > 1e-4f) {
    const float k = clampf(config_.sharpness, 0.0f, 1.0f) * 0.25f;
    for (std::uint32_t oy = 0; oy < inputs.output_height; ++oy) {
      for (std::uint32_t ox = 0; ox < inputs.output_width; ++ox) {
        const std::size_t idx =
            static_cast<std::size_t>(oy) * inputs.output_width + ox;
        const ColorRgb c = history_color_[idx];
        const ColorRgb n = sample_clamped(history_color_.data(),
                                          inputs.output_width,
                                          inputs.output_height,
                                          static_cast<int>(ox),
                                          static_cast<int>(oy) - 1);
        const ColorRgb s = sample_clamped(history_color_.data(),
                                          inputs.output_width,
                                          inputs.output_height,
                                          static_cast<int>(ox),
                                          static_cast<int>(oy) + 1);
        const ColorRgb w = sample_clamped(history_color_.data(),
                                          inputs.output_width,
                                          inputs.output_height,
                                          static_cast<int>(ox) - 1,
                                          static_cast<int>(oy));
        const ColorRgb e = sample_clamped(history_color_.data(),
                                          inputs.output_width,
                                          inputs.output_height,
                                          static_cast<int>(ox) + 1,
                                          static_cast<int>(oy));

        const float min_r = std::min({c.r, n.r, s.r, w.r, e.r});
        const float max_r = std::max({c.r, n.r, s.r, w.r, e.r});
        const float min_g = std::min({c.g, n.g, s.g, w.g, e.g});
        const float max_g = std::max({c.g, n.g, s.g, w.g, e.g});
        const float min_b = std::min({c.b, n.b, s.b, w.b, e.b});
        const float max_b = std::max({c.b, n.b, s.b, w.b, e.b});

        const ColorRgb blur{(n.r + s.r + w.r + e.r) * 0.25f,
                            (n.g + s.g + w.g + e.g) * 0.25f,
                            (n.b + s.b + w.b + e.b) * 0.25f};
        out_color[idx] = {
            std::clamp(c.r + (c.r - blur.r) * k, min_r, max_r),
            std::clamp(c.g + (c.g - blur.g) * k, min_g, max_g),
            std::clamp(c.b + (c.b - blur.b) * k, min_b, max_b),
        };
      }
    }
  } else {
    out_color = history_color_;
  }

  metrics.valid = true;
  const auto t1 = std::chrono::steady_clock::now();
  metrics.cpu_time_ms =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
  return metrics;
}

void upscale_bilinear(const ColorRgb* src, std::uint32_t src_w, std::uint32_t src_h,
                      std::uint32_t dst_w, std::uint32_t dst_h,
                      std::vector<ColorRgb>& out) {
  out.resize(static_cast<std::size_t>(dst_w) * dst_h);
  if (src == nullptr || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
    return;
  }
  const float sx = static_cast<float>(src_w) / static_cast<float>(dst_w);
  const float sy = static_cast<float>(src_h) / static_cast<float>(dst_h);
  for (std::uint32_t y = 0; y < dst_h; ++y) {
    for (std::uint32_t x = 0; x < dst_w; ++x) {
      const float fx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
      const float fy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
      out[static_cast<std::size_t>(y) * dst_w + x] =
          sample_bilinear(src, src_w, src_h, fx, fy);
    }
  }
}

void upscale_bicubic(const ColorRgb* src, std::uint32_t src_w, std::uint32_t src_h,
                     std::uint32_t dst_w, std::uint32_t dst_h,
                     std::vector<ColorRgb>& out) {
  out.resize(static_cast<std::size_t>(dst_w) * dst_h);
  if (src == nullptr || src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
    return;
  }
  const float sx = static_cast<float>(src_w) / static_cast<float>(dst_w);
  const float sy = static_cast<float>(src_h) / static_cast<float>(dst_h);
  for (std::uint32_t y = 0; y < dst_h; ++y) {
    for (std::uint32_t x = 0; x < dst_w; ++x) {
      const float fx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
      const float fy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
      out[static_cast<std::size_t>(y) * dst_w + x] =
          sample_catmull_rom(src, src_w, src_h, fx, fy);
    }
  }
}

}  // namespace saige::ssr
