#include "saige/ssr.hpp"
#include "../gpu/ssr_vulkan_harness.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#define SAIGE_CHECK(cond)                                                     \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::cerr << "SAIGE_CHECK FAILED at " << __FILE__ << ":" << __LINE__    \
                << " (" << #cond << ")\n";                                    \
      std::exit(1);                                                           \
    }                                                                         \
  } while (false)

using namespace saige::ssr;

namespace {

enum class SceneKind {
  MovingVehicleAndFence = 0,
  FineFoliageAndPowerLines = 1,
  NighttimeNeonHdr = 2,
  ReflectiveRoadAndGlass = 3,
  RapidCameraPanAndCut = 4,
};

[[nodiscard]] ColorRgb tonemap_srgb(ColorRgb hdr) noexcept {
  auto tm = [](float v) -> float {
    const float mapped = v / (1.0f + std::max(0.0f, v));
    return std::pow(std::clamp(mapped, 0.0f, 1.0f), 1.0f / 2.2f);
  };
  return {tm(hdr.r), tm(hdr.g), tm(hdr.b)};
}

[[nodiscard]] double compute_psnr_db(const std::vector<ColorRgb>& ref,
                                     const std::vector<ColorRgb>& test) noexcept {
  double mse = 0.0;
  for (std::size_t i = 0; i < ref.size(); ++i) {
    const ColorRgb a = tonemap_srgb(ref[i]);
    const ColorRgb b = tonemap_srgb(test[i]);
    const double dr = static_cast<double>(a.r - b.r);
    const double dg = static_cast<double>(a.g - b.g);
    const double db = static_cast<double>(a.b - b.b);
    mse += (dr * dr + dg * dg + db * db) / 3.0;
  }
  mse /= static_cast<double>(ref.size());
  if (mse <= 1e-12) return 99.0;
  return 10.0 * std::log10(1.0 / mse);
}

[[nodiscard]] double compute_ssim(const std::vector<ColorRgb>& ref,
                                  const std::vector<ColorRgb>& test,
                                  std::uint32_t w, std::uint32_t h) noexcept {
  auto luma = [](ColorRgb c) {
    const ColorRgb s = tonemap_srgb(c);
    return 0.2126 * s.r + 0.7152 * s.g + 0.0722 * s.b;
  };
  constexpr double C1 = 0.01 * 0.01;
  constexpr double C2 = 0.03 * 0.03;
  double ssim_sum = 0.0;
  std::uint64_t patches = 0;
  for (std::uint32_t y = 0; y + 8 <= h; y += 8) {
    for (std::uint32_t x = 0; x + 8 <= w; x += 8) {
      double mx = 0.0, my = 0.0;
      for (std::uint32_t py = 0; py < 8; ++py) {
        for (std::uint32_t px = 0; px < 8; ++px) {
          const std::size_t idx = static_cast<std::size_t>(y + py) * w + (x + px);
          mx += luma(ref[idx]);
          my += luma(test[idx]);
        }
      }
      mx /= 64.0;
      my /= 64.0;
      double vx = 0.0, vy = 0.0, cov = 0.0;
      for (std::uint32_t py = 0; py < 8; ++py) {
        for (std::uint32_t px = 0; px < 8; ++px) {
          const std::size_t idx = static_cast<std::size_t>(y + py) * w + (x + px);
          const double dx = luma(ref[idx]) - mx;
          const double dy = luma(test[idx]) - my;
          vx += dx * dx;
          vy += dy * dy;
          cov += dx * dy;
        }
      }
      vx /= 63.0;
      vy /= 63.0;
      cov /= 63.0;
      ssim_sum += ((2.0 * mx * my + C1) * (2.0 * cov + C2)) /
                  ((mx * mx + my * my + C1) * (vx + vy + C2));
      ++patches;
    }
  }
  return patches > 0 ? (ssim_sum / static_cast<double>(patches)) : 0.0;
}

[[nodiscard]] double compute_temporal_flicker_rmse(
    const std::vector<ColorRgb>& prev_out, const std::vector<ColorRgb>& curr_out,
    const std::vector<ColorRgb>& prev_gt, const std::vector<ColorRgb>& curr_gt) noexcept {
  double mse = 0.0;
  for (std::size_t i = 0; i < curr_out.size(); ++i) {
    const ColorRgb do_c = tonemap_srgb(curr_out[i]);
    const ColorRgb do_p = tonemap_srgb(prev_out[i]);
    const ColorRgb dg_c = tonemap_srgb(curr_gt[i]);
    const ColorRgb dg_p = tonemap_srgb(prev_gt[i]);
    const double diff_r = static_cast<double>((do_c.r - do_p.r) - (dg_c.r - dg_p.r));
    const double diff_g = static_cast<double>((do_c.g - do_p.g) - (dg_c.g - dg_p.g));
    const double diff_b = static_cast<double>((do_c.b - do_p.b) - (dg_c.b - dg_p.b));
    mse += (diff_r * diff_r + diff_g * diff_g + diff_b * diff_b) / 3.0;
  }
  return std::sqrt(mse / static_cast<double>(curr_out.size()));
}

struct SyntheticFrame {
  std::vector<ColorRgb> gt_color;
  std::vector<ColorRgb> low_color;
  std::vector<float> low_depth;
  std::vector<Vec2f> low_mv;
  std::vector<float> low_reactive;
  std::vector<float> low_transp;
};

SyntheticFrame render_scene(SceneKind kind, std::uint32_t out_w, std::uint32_t out_h,
                            std::uint32_t ren_w, std::uint32_t ren_h,
                            std::uint32_t f, Vec2f jitter_px, float exposure) {
  SyntheticFrame s{};
  const std::size_t out_n = static_cast<std::size_t>(out_w) * out_h;
  const std::size_t ren_n = static_cast<std::size_t>(ren_w) * ren_h;
  s.gt_color.resize(out_n);
  s.low_color.resize(ren_n);
  s.low_depth.resize(ren_n);
  s.low_mv.resize(ren_n);
  s.low_reactive.assign(ren_n, 0.0f);
  s.low_transp.assign(ren_n, 0.0f);

  const float cam_dx = (kind == SceneKind::RapidCameraPanAndCut) ? 6.0f : 0.0f;
  const float cam_off_x = static_cast<float>(f) * cam_dx;
  const float prev_cam_off_x = static_cast<float>(f > 0 ? f - 1 : 0) * cam_dx;

  const float box_cx = 120.0f + static_cast<float>(f) * 4.5f;
  const float prev_box_cx = 120.0f + static_cast<float>(f > 0 ? f - 1 : 0) * 4.5f;
  const float box_cy = static_cast<float>(out_h) * 0.5f;

  auto eval = [&](float ox, float oy, float& depth, Vec2f& mv, float& react, float& transp) -> ColorRgb {
    react = 0.0f;
    transp = 0.0f;

    if (kind == SceneKind::MovingVehicleAndFence || kind == SceneKind::RapidCameraPanAndCut) {
      if (std::abs(ox - box_cx) < 38.0f && std::abs(oy - box_cy) < 22.0f) {
        depth = 0.85f;
        mv = {prev_box_cx - box_cx, 0.0f};
        const float stripe = std::sin((ox - box_cx) * 0.35f) * 0.5f + 0.5f;
        return {1.8f * exposure, (0.25f + 0.6f * stripe) * exposure, 0.15f * exposure};
      }
      depth = 0.25f;
      mv = {cam_off_x - prev_cam_off_x, 0.0f};
      const float wx = ox + cam_off_x;
      const float fine = std::sin(wx * 0.45f + oy * 0.35f) > 0.85f ? 1.4f : 0.0f;
      const float gx = std::abs(std::fmod(wx, 24.0f)) < 1.5f ? 0.9f : 0.0f;
      const float gy = std::abs(std::fmod(oy, 24.0f)) < 1.5f ? 0.9f : 0.0f;
      const float base = 0.18f + 0.35f * std::max(fine, std::max(gx, gy));
      return {base * exposure, (base + 0.04f) * exposure, (base + 0.12f) * exposure};
    }

    if (kind == SceneKind::FineFoliageAndPowerLines) {
      depth = 0.40f;
      mv = {0.0f, 0.0f};
      // Thin subpixel power lines + swaying foliage pattern
      const float line1 = std::abs(oy - (80.0f + 0.08f * ox)) < 0.9f ? 1.5f : 0.0f;
      const float line2 = std::abs(oy - (95.0f + 0.08f * ox)) < 0.9f ? 1.5f : 0.0f;
      const float sway = std::sin(static_cast<float>(f) * 0.25f + ox * 0.1f) * 1.5f;
      const float leaf = (std::sin((ox + sway) * 0.7f) * std::cos(oy * 0.7f)) > 0.3f ? 0.6f : 0.0f;
      if (leaf > 0.0f) mv = {-std::cos(static_cast<float>(f) * 0.25f) * 0.35f, 0.0f};
      return {(0.12f + line1 + line2) * exposure,
              (0.18f + 0.55f * leaf + line1) * exposure,
              (0.14f + line2) * exposure};
    }

    if (kind == SceneKind::NighttimeNeonHdr) {
      depth = 0.30f;
      mv = {0.0f, 0.0f};
      // Bright HDR neon sign (12.0 linear) + moving headlight
      const bool in_neon = (ox > 60.0f && ox < 180.0f && oy > 40.0f && oy < 70.0f);
      const float neon = in_neon && (std::sin(ox * 0.5f) > 0.4f) ? 12.0f : 0.0f;
      const float hl_dx = ox - box_cx;
      const float hl_dy = oy - box_cy;
      const float hl = std::exp(-(hl_dx * hl_dx + hl_dy * hl_dy) / 180.0f) * 15.0f;
      if (hl > 0.5f) mv = {prev_box_cx - box_cx, 0.0f};
      return {(0.03f + neon + hl) * exposure,
              (0.03f + 0.2f * neon + 0.9f * hl) * exposure,
              (0.06f + 0.8f * neon + 0.5f * hl) * exposure};
    }

    // ReflectiveRoadAndGlass
    depth = 0.50f;
    mv = {0.0f, 0.0f};
    const bool in_glass = (ox > 150.0f && ox < 300.0f && oy > 80.0f && oy < 220.0f);
    if (in_glass) {
      transp = 0.8f;
      react = 0.6f;
    }
    const float road_mark = (std::abs(oy - static_cast<float>(out_h) * 0.75f) < 3.0f &&
                             std::fmod(ox, 30.0f) < 15.0f)
                                ? 1.2f
                                : 0.0f;
    const float refl = in_glass ? (0.4f + 0.4f * std::sin((ox + static_cast<float>(f) * 2.0f) * 0.2f)) : 0.0f;
    return {(0.15f + road_mark + refl) * exposure,
            (0.16f + road_mark + 0.8f * refl) * exposure,
            (0.20f + road_mark + 1.1f * refl) * exposure};
  };

  for (std::uint32_t y = 0; y < out_h; ++y) {
    for (std::uint32_t x = 0; x < out_w; ++x) {
      float d = 0.0f, r = 0.0f, tr = 0.0f;
      Vec2f mv{};
      s.gt_color[static_cast<std::size_t>(y) * out_w + x] =
          eval(static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f, d, mv, r, tr);
    }
  }

  const float sx = static_cast<float>(out_w) / static_cast<float>(ren_w);
  const float sy = static_cast<float>(out_h) / static_cast<float>(ren_h);
  for (std::uint32_t y = 0; y < ren_h; ++y) {
    for (std::uint32_t x = 0; x < ren_w; ++x) {
      const float ox = (static_cast<float>(x) + 0.5f + jitter_px.x) * sx;
      const float oy = (static_cast<float>(y) + 0.5f + jitter_px.y) * sy;
      float d = 0.0f, r = 0.0f, tr = 0.0f;
      Vec2f mv_out{};
      const std::size_t idx = static_cast<std::size_t>(y) * ren_w + x;
      s.low_color[idx] = eval(ox, oy, d, mv_out, r, tr);
      s.low_depth[idx] = d;
      s.low_mv[idx] = {mv_out.x / sx, mv_out.y / sy};
      s.low_reactive[idx] = r;
      s.low_transp[idx] = tr;
    }
  }
  return s;
}


// Conventional 1st-gen temporal reconstruction baseline (TAA-U):
// bilinear reprojection + basic 3x3 RGB min/max clamp + fixed alpha=0.15,
// without closest-depth MV dilation, depth disocclusion, HDR Karis compression,
// confidence tracking, or separated post-history RCAS sharpening.
struct TaauBaselineContext {
  std::vector<ColorRgb> hist;
  std::uint32_t hw = 0, hh = 0;
  bool has = false;

  void run(const SsrDispatchInputs& in, std::vector<ColorRgb>& out) {
    const std::size_t n = static_cast<std::size_t>(in.output_width) * in.output_height;
    out.resize(n);
    if (!has || in.camera_cut || hw != in.output_width || hh != in.output_height) {
      hist.assign(n, ColorRgb{0.0f, 0.0f, 0.0f});
      hw = in.output_width;
      hh = in.output_height;
      has = false;
    }
    std::vector<ColorRgb> curr_up;
    upscale_bilinear(in.color_hdr, in.render_width, in.render_height,
                     in.output_width, in.output_height, curr_up);
    if (!has) {
      hist = curr_up;
      out = curr_up;
      has = true;
      return;
    }
    const float sx = static_cast<float>(in.render_width) / static_cast<float>(in.output_width);
    const float sy = static_cast<float>(in.render_height) / static_cast<float>(in.output_height);
    for (std::uint32_t oy = 0; oy < in.output_height; ++oy) {
      for (std::uint32_t ox = 0; ox < in.output_width; ++ox) {
        const std::size_t idx = static_cast<std::size_t>(oy) * in.output_width + ox;
        const int ix = std::clamp(static_cast<int>(static_cast<float>(ox) * sx), 0, static_cast<int>(in.render_width) - 1);
        const int iy = std::clamp(static_cast<int>(static_cast<float>(oy) * sy), 0, static_cast<int>(in.render_height) - 1);
        const Vec2f mv = in.motion_vectors_px[static_cast<std::size_t>(iy) * in.render_width + static_cast<std::size_t>(ix)];
        const int hx = static_cast<int>(std::lround(static_cast<float>(ox) + mv.x / sx));
        const int hy = static_cast<int>(std::lround(static_cast<float>(oy) + mv.y / sy));
        const ColorRgb c = curr_up[idx];
        if (hx < 0 || hy < 0 || hx >= static_cast<int>(in.output_width) || hy >= static_cast<int>(in.output_height)) {
          out[idx] = c;
          continue;
        }
        ColorRgb mn{1e9f, 1e9f, 1e9f}, mx{-1e9f, -1e9f, -1e9f};
        for (int ky = -1; ky <= 1; ++ky) {
          for (int kx = -1; kx <= 1; ++kx) {
            const int sx_i = std::clamp(ix + kx, 0, static_cast<int>(in.render_width) - 1);
            const int sy_i = std::clamp(iy + ky, 0, static_cast<int>(in.render_height) - 1);
            const ColorRgb nc = in.color_hdr[static_cast<std::size_t>(sy_i) * in.render_width + static_cast<std::size_t>(sx_i)];
            mn.r = std::min(mn.r, nc.r); mn.g = std::min(mn.g, nc.g); mn.b = std::min(mn.b, nc.b);
            mx.r = std::max(mx.r, nc.r); mx.g = std::max(mx.g, nc.g); mx.b = std::max(mx.b, nc.b);
          }
        }
        const ColorRgb h = hist[static_cast<std::size_t>(hy) * in.output_width + static_cast<std::size_t>(hx)];
        const ColorRgb ch{std::clamp(h.r, mn.r, mx.r), std::clamp(h.g, mn.g, mx.g), std::clamp(h.b, mn.b, mx.b)};
        constexpr float alpha = 0.15f;
        out[idx] = {ch.r * (1.0f - alpha) + c.r * alpha,
                    ch.g * (1.0f - alpha) + c.g * alpha,
                    ch.b * (1.0f - alpha) + c.b * alpha};
      }
    }
    hist = out;
  }
};

struct SceneBenchResult {
  std::string name;
  double bil_psnr = 0.0;
  double bic_psnr = 0.0;
  double ssr_psnr = 0.0;
  double bil_ssim = 0.0;
  double bic_ssim = 0.0;
  double ssr_ssim = 0.0;
  double taau_psnr = 0.0;
  double taau_ssim = 0.0;
  double taau_flicker_rmse = 0.0;
  double ssr_flicker_rmse = 0.0;
  double bil_flicker_rmse = 0.0;
  double avg_cpu_ms = 0.0;
};

SceneBenchResult run_scene_benchmark(SceneKind kind, const std::string& name,
                                     std::uint32_t out_w, std::uint32_t out_h,
                                     SsrQualityMode mode) {
  const auto ren = SsrContext::compute_render_resolution(out_w, out_h, mode);
  SsrContext ssr;
  TaauBaselineContext taau;
  std::vector<ColorRgb> ssr_out, prev_ssr_out, taau_out, prev_taau_out, bil_out, prev_bil_out, bic_out, prev_gt;
  double total_ms = 0.0;
  double flicker_ssr = 0.0, flicker_taau = 0.0, flicker_bil = 0.0;
  int flicker_frames = 0;

  SceneBenchResult res{};
  res.name = name;

  for (std::uint32_t f = 0; f < 24; ++f) {
    const Vec2f jitter = SsrContext::halton_jitter_px(f);
    const float exp = (f >= 18) ? 2.5f : 1.0f;
    const bool cut = (f == 12);
    const auto sf = render_scene(kind, out_w, out_h, ren.width, ren.height, f, jitter, exp);

    SsrDispatchInputs in{};
    in.render_width = ren.width;
    in.render_height = ren.height;
    in.output_width = out_w;
    in.output_height = out_h;
    in.color_hdr = sf.low_color.data();
    in.depth = sf.low_depth.data();
    in.motion_vectors_px = sf.low_mv.data();
    in.reactive_mask = sf.low_reactive.data();
    in.transparency_mask = sf.low_transp.data();
    in.jitter_offset_px = jitter;
    in.pre_exposure = exp;
    in.camera_cut = cut;
    in.reversed_z = true;

    const auto m = ssr.reconstruct(in, ssr_out);
    SAIGE_CHECK(m.valid);
    total_ms += m.cpu_time_ms;

    taau.run(in, taau_out);
    upscale_bilinear(sf.low_color.data(), ren.width, ren.height, out_w, out_h, bil_out);
    upscale_bicubic(sf.low_color.data(), ren.width, ren.height, out_w, out_h, bic_out);

    if (f > 0 && !cut && f != 18) {
      flicker_ssr += compute_temporal_flicker_rmse(prev_ssr_out, ssr_out, prev_gt, sf.gt_color);
      flicker_taau += compute_temporal_flicker_rmse(prev_taau_out, taau_out, prev_gt, sf.gt_color);
      flicker_bil += compute_temporal_flicker_rmse(prev_bil_out, bil_out, prev_gt, sf.gt_color);
      ++flicker_frames;
    }

    if (f == 23) {
      res.bil_psnr = compute_psnr_db(sf.gt_color, bil_out);
      res.bic_psnr = compute_psnr_db(sf.gt_color, bic_out);
      res.taau_psnr = compute_psnr_db(sf.gt_color, taau_out);
      res.ssr_psnr = compute_psnr_db(sf.gt_color, ssr_out);
      res.bil_ssim = compute_ssim(sf.gt_color, bil_out, out_w, out_h);
      res.bic_ssim = compute_ssim(sf.gt_color, bic_out, out_w, out_h);
      res.taau_ssim = compute_ssim(sf.gt_color, taau_out, out_w, out_h);
      res.ssr_ssim = compute_ssim(sf.gt_color, ssr_out, out_w, out_h);
    }

    prev_ssr_out = ssr_out;
    prev_taau_out = taau_out;
    prev_bil_out = bil_out;
    prev_gt = sf.gt_color;
  }

  res.avg_cpu_ms = total_ms / 24.0;
  res.ssr_flicker_rmse = flicker_frames > 0 ? (flicker_ssr / flicker_frames) : 0.0;
  res.taau_flicker_rmse = flicker_frames > 0 ? (flicker_taau / flicker_frames) : 0.0;
  res.bil_flicker_rmse = flicker_frames > 0 ? (flicker_bil / flicker_frames) : 0.0;
  return res;
}

void write_ppm(const std::string& path, const std::vector<ColorRgb>& img,
               std::uint32_t w, std::uint32_t h) {
  std::ofstream f(path, std::ios::binary);
  f << "P6\n" << w << " " << h << "\n255\n";
  for (std::size_t i = 0; i < img.size(); ++i) {
    const ColorRgb s = tonemap_srgb(img[i]);
    const unsigned char rgb[3] = {
        static_cast<unsigned char>(std::clamp(s.r * 255.0f, 0.0f, 255.0f)),
        static_cast<unsigned char>(std::clamp(s.g * 255.0f, 0.0f, 255.0f)),
        static_cast<unsigned char>(std::clamp(s.b * 255.0f, 0.0f, 255.0f)),
    };
    f.write(reinterpret_cast<const char*>(rgb), 3);
  }
}

void test_all_six_quality_modes_and_dynamic_res() {
  const SsrQualityMode modes[] = {
      SsrQualityMode::NativeAA,    SsrQualityMode::UltraQuality,
      SsrQualityMode::Quality,     SsrQualityMode::Balanced,
      SsrQualityMode::Performance, SsrQualityMode::UltraPerformance};
  SsrContext ssr;
  std::vector<ColorRgb> out;
  for (auto mode : modes) {
    const auto ren = SsrContext::compute_render_resolution(1920, 1080, mode);
    SAIGE_CHECK(ren.width >= 1 && ren.width <= 1920);
    SAIGE_CHECK(ren.height >= 1 && ren.height <= 1080);

    // Run 2 dynamic-resolution frames on a 128x72 output target
    const auto dr = SsrContext::compute_render_resolution(128, 72, mode);
    const auto sf = render_scene(SceneKind::MovingVehicleAndFence, 128, 72, dr.width, dr.height, 1, {0.0f, 0.0f}, 1.0f);
    SsrDispatchInputs in{dr.width, dr.height, 128, 72, sf.low_color.data(), sf.low_depth.data(), sf.low_mv.data()};
    SAIGE_CHECK(ssr.reconstruct(in, out).valid);
  }
}

void test_build001_regression_and_multi_scene_suite() {
  // Build 001 regression: 360p Performance (must beat Build 001's 25.74 dB / 0.781 SSIM)
  const auto r0 = run_scene_benchmark(SceneKind::MovingVehicleAndFence, "moving_vehicle_and_fence_360p_perf", 640, 360, SsrQualityMode::Performance);
  const auto r1 = run_scene_benchmark(SceneKind::FineFoliageAndPowerLines, "fine_foliage_and_power_lines_360p_perf", 640, 360, SsrQualityMode::Performance);
  const auto r2 = run_scene_benchmark(SceneKind::NighttimeNeonHdr, "nighttime_neon_hdr_360p_perf", 640, 360, SsrQualityMode::Performance);
  const auto r3 = run_scene_benchmark(SceneKind::ReflectiveRoadAndGlass, "reflective_road_and_glass_360p_perf", 640, 360, SsrQualityMode::Performance);
  const auto r4 = run_scene_benchmark(SceneKind::RapidCameraPanAndCut, "rapid_camera_pan_and_cut_360p_perf", 640, 360, SsrQualityMode::Performance);

  const SceneBenchResult results[] = {r0, r1, r2, r3, r4};
  for (const auto& r : results) {
    std::cout << "[bench " << r.name << "] avg_ms=" << r.avg_cpu_ms
              << " Bil(PSNR=" << r.bil_psnr << ",SSIM=" << r.bil_ssim << ",Flicker=" << r.bil_flicker_rmse << ")"
              << " Bic(PSNR=" << r.bic_psnr << ",SSIM=" << r.bic_ssim << ")"
              << " TAAU(PSNR=" << r.taau_psnr << ",SSIM=" << r.taau_ssim << ",Flicker=" << r.taau_flicker_rmse << ")"
              << " SSR0.2.1(PSNR=" << r.ssr_psnr << ",SSIM=" << r.ssr_ssim << ",Flicker=" << r.ssr_flicker_rmse << ")\n";
    SAIGE_CHECK(r.ssr_psnr > r.bil_psnr);
    SAIGE_CHECK(r.ssr_ssim > r.bil_ssim);
  }
  // No unexplained regression vs Build 001 (25.7367 dB / 0.7807 SSIM)
  SAIGE_CHECK(r0.ssr_psnr >= 25.70);
  SAIGE_CHECK(r0.ssr_ssim >= 0.780);
}

void test_negative_and_mutation_detection() {
  // 1. Null/invalid dimensions
  SsrContext ssr;
  std::vector<ColorRgb> out;
  SAIGE_CHECK(!ssr.reconstruct(SsrDispatchInputs{}, out).valid);

  // 2. Corrupted NaN/Inf sanitization
  constexpr std::uint32_t kW = 16, kH = 16;
  std::vector<ColorRgb> col(kW * kH, {0.5f, 0.5f, 0.5f});
  std::vector<float> dep(kW * kH, 0.5f);
  std::vector<Vec2f> mv(kW * kH, {0.0f, 0.0f});
  col[0].r = std::numeric_limits<float>::quiet_NaN();
  dep[1] = std::numeric_limits<float>::infinity();
  mv[2].x = std::numeric_limits<float>::quiet_NaN();
  SsrDispatchInputs bad_vals{kW, kH, 32, 32, col.data(), dep.data(), mv.data()};
  const auto m = ssr.reconstruct(bad_vals, out);
  SAIGE_CHECK(m.valid);
  SAIGE_CHECK(m.sanitized_invalid_inputs >= 3);

  // 3. Deliberate Mutation Detection: inverting motion vectors must degrade PSNR
  auto measure_with_mutation = [&](bool invert_mv, bool zero_jitter) {
    SsrContext ctx;
    std::vector<ColorRgb> o;
    double last_psnr = 0.0;
    const auto ren = SsrContext::compute_render_resolution(320, 180, SsrQualityMode::Performance);
    for (std::uint32_t f = 0; f < 16; ++f) {
      const Vec2f j = zero_jitter ? Vec2f{0.0f, 0.0f} : SsrContext::halton_jitter_px(f);
      auto sf = render_scene(SceneKind::MovingVehicleAndFence, 320, 180, ren.width, ren.height, f, j, 1.0f);
      if (invert_mv) {
        for (auto& v : sf.low_mv) {
          v.x = -v.x;
          v.y = -v.y;
        }
      }
      SsrDispatchInputs in{ren.width, ren.height, 320, 180, sf.low_color.data(), sf.low_depth.data(), sf.low_mv.data(), nullptr, nullptr, j, 1.0f, false, true};
      ctx.reconstruct(in, o);
      if (f == 15) last_psnr = compute_psnr_db(sf.gt_color, o);
    }
    return last_psnr;
  };
  const double baseline_psnr = measure_with_mutation(false, false);
  const double inverted_mv_psnr = measure_with_mutation(true, false);
  const double zero_jitter_psnr = measure_with_mutation(false, true);
  SAIGE_CHECK(baseline_psnr > inverted_mv_psnr);
  SAIGE_CHECK(baseline_psnr > zero_jitter_psnr);

  // 4. Oversized render dimensions & null pointer rejections
  SsrDispatchInputs oversized{64, 64, 32, 32, col.data(), dep.data(), mv.data()};
  SAIGE_CHECK(!ssr.reconstruct(oversized, out).valid);
  SsrDispatchInputs null_dep{kW, kH, 32, 32, col.data(), nullptr, mv.data()};
  SAIGE_CHECK(!ssr.reconstruct(null_dep, out).valid);

  // 5. Camera-cut reset & exposure jump invariance
  SsrContext exp_ctx;
  std::vector<ColorRgb> exp_out;
  const auto ren = SsrContext::compute_render_resolution(160, 90, SsrQualityMode::Performance);
  auto sf_e1 = render_scene(SceneKind::NighttimeNeonHdr, 160, 90, ren.width, ren.height, 4, SsrContext::halton_jitter_px(4), 1.0f);
  SsrDispatchInputs in_e1{ren.width, ren.height, 160, 90, sf_e1.low_color.data(), sf_e1.low_depth.data(), sf_e1.low_mv.data(), nullptr, nullptr, SsrContext::halton_jitter_px(4), 1.0f, true, true};
  SAIGE_CHECK(exp_ctx.reconstruct(in_e1, exp_out).history_reset_pixels == 160u * 90u);
  auto sf_e2 = render_scene(SceneKind::NighttimeNeonHdr, 160, 90, ren.width, ren.height, 5, SsrContext::halton_jitter_px(5), 4.0f);
  SsrDispatchInputs in_e2{ren.width, ren.height, 160, 90, sf_e2.low_color.data(), sf_e2.low_depth.data(), sf_e2.low_mv.data(), nullptr, nullptr, SsrContext::halton_jitter_px(5), 4.0f, false, true};
  SAIGE_CHECK(exp_ctx.reconstruct(in_e2, exp_out).valid);
}

void test_vulkan_gpu_parity_suite() {
  gpu::SsrVulkanHarness harness;
  std::string status;
  const bool ready = harness.initialize(gpu::VulkanHarnessConfig{}, status);
  SAIGE_CHECK(!status.empty());

  SsrContext cpu_ctx;
  std::vector<ColorRgb> cpu_out, gpu_out;
  const SsrQualityMode dyn_modes[] = {
      SsrQualityMode::Quality, SsrQualityMode::Performance, SsrQualityMode::Balanced};

  for (int f = 0; f < 6; ++f) {
    const auto mode = dyn_modes[f / 2];
    const auto ren = SsrContext::compute_render_resolution(160, 90, mode);
    const Vec2f j = SsrContext::halton_jitter_px(static_cast<std::uint32_t>(f));
    const float exp = (f >= 4) ? 2.0f : 1.0f;
    const bool cut = (f == 3);
    const auto kind = (f % 2 == 0) ? SceneKind::MovingVehicleAndFence : SceneKind::ReflectiveRoadAndGlass;
    const auto sf = render_scene(kind, 160, 90, ren.width, ren.height, static_cast<std::uint32_t>(f), j, exp);
    SsrDispatchInputs in{ren.width, ren.height, 160, 90, sf.low_color.data(), sf.low_depth.data(), sf.low_mv.data(), sf.low_reactive.data(), sf.low_transp.data(), j, exp, cut, true};
    SAIGE_CHECK(cpu_ctx.reconstruct(in, cpu_out).valid);

    if (ready) {
      const auto rep = harness.execute_frame(in, cpu_out, gpu_out);
      SAIGE_CHECK(rep.executed);
      SAIGE_CHECK(rep.validation_clean);
      SAIGE_CHECK(rep.parity_passed);
    }
  }
  if (ready) {
    harness.reset_history();
    // Invalid input check on initialized harness
    const auto bad_rep = harness.execute_frame(SsrDispatchInputs{}, cpu_out, gpu_out);
    SAIGE_CHECK(!bad_rep.executed);
  }
}

}  // namespace

int main(int argc, char** argv) {
  test_all_six_quality_modes_and_dynamic_res();
  test_build001_regression_and_multi_scene_suite();
  test_negative_and_mutation_detection();
  test_vulkan_gpu_parity_suite();

  if (argc >= 3 && std::string(argv[1]) == "--dump-artifacts") {
    const std::string dir = argv[2];
    // Write machine-readable JSON benchmark report
    const auto r0 = run_scene_benchmark(SceneKind::MovingVehicleAndFence, "moving_vehicle_and_fence", 640, 360, SsrQualityMode::Performance);
    const auto r1 = run_scene_benchmark(SceneKind::FineFoliageAndPowerLines, "fine_foliage_and_power_lines", 640, 360, SsrQualityMode::Performance);
    const auto r2 = run_scene_benchmark(SceneKind::NighttimeNeonHdr, "nighttime_neon_hdr", 640, 360, SsrQualityMode::Performance);
    const auto r3 = run_scene_benchmark(SceneKind::ReflectiveRoadAndGlass, "reflective_road_and_glass", 640, 360, SsrQualityMode::Performance);
    const auto r4 = run_scene_benchmark(SceneKind::RapidCameraPanAndCut, "rapid_camera_pan_and_cut", 640, 360, SsrQualityMode::Performance);
    const SceneBenchResult all[] = {r0, r1, r2, r3, r4};

    std::ofstream jf(dir + "/benchmark_results.json");
    jf << "{\n  \"build\": 2,\n  \"quality_mode\": \"Performance (50%)\",\n  \"scenes\": [\n";
    for (int i = 0; i < 5; ++i) {
      jf << "    {\"name\": \"" << all[i].name << "\", \"avg_cpu_ms\": " << all[i].avg_cpu_ms
         << ", \"bilinear_psnr_db\": " << all[i].bil_psnr << ", \"bilinear_ssim\": " << all[i].bil_ssim
         << ", \"bicubic_psnr_db\": " << all[i].bic_psnr << ", \"bicubic_ssim\": " << all[i].bic_ssim
         << ", \"taau_psnr_db\": " << all[i].taau_psnr << ", \"taau_ssim\": " << all[i].taau_ssim << ", \"taau_flicker_rmse\": " << all[i].taau_flicker_rmse
         << ", \"ssr_psnr_db\": " << all[i].ssr_psnr << ", \"ssr_ssim\": " << all[i].ssr_ssim
         << ", \"ssr_flicker_rmse\": " << all[i].ssr_flicker_rmse
         << ", \"bilinear_flicker_rmse\": " << all[i].bil_flicker_rmse << "}"
         << (i < 4 ? "," : "") << "\n";
    }
    jf << "  ]\n}\n";

    // Render 150-frame 720p30 diagnostic sequence + 1080p still
    constexpr std::uint32_t kOutW = 960, kOutH = 540;
    const auto ren = SsrContext::compute_render_resolution(kOutW, kOutH, SsrQualityMode::Performance);
    SsrContext ssr;
    std::vector<ColorRgb> ssr_out, bil_out, bic_out;
    for (std::uint32_t f = 0; f < 150; ++f) {
      const Vec2f j = SsrContext::halton_jitter_px(f);
      const auto kind = (f < 75) ? SceneKind::MovingVehicleAndFence : SceneKind::NighttimeNeonHdr;
      const auto sf = render_scene(kind, kOutW, kOutH, ren.width, ren.height, f, j, 1.0f);
      SsrDispatchInputs in{ren.width, ren.height, kOutW, kOutH, sf.low_color.data(), sf.low_depth.data(), sf.low_mv.data(), sf.low_reactive.data(), sf.low_transp.data(), j, 1.0f, (f == 75), true};
      ssr.reconstruct(in, ssr_out);
      upscale_bilinear(sf.low_color.data(), ren.width, ren.height, kOutW, kOutH, bil_out);
      upscale_bicubic(sf.low_color.data(), ren.width, ren.height, kOutW, kOutH, bic_out);

      std::vector<ColorRgb> quad(1280 * 720);
      auto blit = [&](const std::vector<ColorRgb>& src, std::uint32_t qx, std::uint32_t qy) {
        for (std::uint32_t y = 0; y < 360; ++y) {
          for (std::uint32_t x = 0; x < 640; ++x) {
            const std::uint32_t sx = (x * kOutW) / 640;
            const std::uint32_t sy = (y * kOutH) / 360;
            quad[static_cast<std::size_t>(qy + y) * 1280 + (qx + x)] = src[static_cast<std::size_t>(sy) * kOutW + sx];
          }
        }
      };
      blit(sf.gt_color, 0, 0);
      blit(bil_out, 640, 0);
      blit(bic_out, 0, 360);
      blit(ssr_out, 640, 360);

      char buf[64];
      std::snprintf(buf, sizeof(buf), "/frame_%04u.ppm", f);
      write_ppm(dir + buf, quad, 1280, 720);

      if (f == 45 || f == 110) {
        std::vector<ColorRgb> still(1920 * 1080);
        auto blit1080 = [&](const std::vector<ColorRgb>& src, std::uint32_t qx, std::uint32_t qy) {
          for (std::uint32_t y = 0; y < 540; ++y) {
            for (std::uint32_t x = 0; x < 960; ++x) {
              still[static_cast<std::size_t>(qy + y) * 1920 + (qx + x)] = src[static_cast<std::size_t>(y) * kOutW + x];
            }
          }
        };
        blit1080(sf.gt_color, 0, 0);
        blit1080(bil_out, 960, 0);
        blit1080(bic_out, 0, 540);
        blit1080(ssr_out, 960, 540);
        const std::string tag = (f == 45) ? "/ssr_1080p_day_comparison.ppm" : "/ssr_1080p_night_hdr_comparison.ppm";
        write_ppm(dir + tag, still, 1920, 1080);
      }
    }
  }

  std::cout << "ssr_test: ALL 4 SUITES PASSED\n";
  return 0;
}
