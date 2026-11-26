// Independent validation of Gemini's SSR Build 002.1 Vulkan compute harness (SAIGE, claude/ssr-validation).
//
//   ssr_validation_test                       the correctness suite (exit 0 = pass, 77 = no Vulkan device)
//   ssr_validation_test --findings            also prints measured algorithm findings (never fails on them)
//   ssr_validation_test --mutant R.spv S.spv  runs the parity sequence through mutated shader binaries; exits 1 when the
//                                             mutant is DETECTED (parity or validation failed), 0 when it survived
//
// Everything runs on whatever Vulkan device the loader offers (llvmpipe on the hosted lanes). None of it is GPU performance evidence.

#include "saige/ssr.hpp"
#include "ssr_vulkan_harness.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace saige::ssr;

namespace {

int g_failures = 0;
#define CHECK(cond)                                                                      \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      std::fprintf(stderr, "CHECK FAILED %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
      ++g_failures;                                                                      \
    }                                                                                    \
  } while (0)
#define CHECK_MSG(cond, ...)                                                             \
  do {                                                                                   \
    if (!(cond)) {                                                                       \
      std::fprintf(stderr, "CHECK FAILED %s:%d  %s : ", __FILE__, __LINE__, #cond);      \
      std::fprintf(stderr, __VA_ARGS__);                                                 \
      std::fprintf(stderr, "\n");                                                        \
      ++g_failures;                                                                      \
    }                                                                                    \
  } while (0)

std::string g_shader_dir = SSR_VALIDATION_SHADER_DIR;
std::string g_recon_override, g_sharp_override;  // --mutant: shader binaries replacing the reconstruct (FP32) and sharpen binaries

// Gates measured on the unmutated shaders on llvmpipe and set with margin (baselines in validation/ssr/README.md). They are tighter than the
// contract's 35 dB / 0.025 because that tolerance only has to cover the bilinear-versus-Catmull-Rom difference, which the tight sequence removes.
constexpr double kScaledRoiMae = 0.0040;   // scaled sequence, region-of-interest MAE (baseline 0.0027)
constexpr double kTightPsnr = 42.0;        // tight sequence, worst frame (baseline 44.5)
constexpr double kTightMae = 0.0022;       // (baseline 0.0015)
constexpr double kTightRoiMae = 0.0014;    // (baseline 0.0010)
constexpr double kTightCutPsnr = 70.0;     // first and cut frames: no history, so GPU and CPU agree to rounding (baseline 79.2)

gpu::VulkanHarnessConfig base_cfg() {
  gpu::VulkanHarnessConfig c;
  c.reconstruct_spv_path = g_shader_dir + "/ssr_reconstruct.spv";
  c.reconstruct_fp16_spv_path = g_shader_dir + "/ssr_reconstruct_fp16.spv";
  c.sharpen_spv_path = g_shader_dir + "/ssr_sharpen.spv";
  c.sharpness = 0.25f;  // the CPU reference's default; the harness default (0.5) would make a parity test compare two different filters
  c.enable_validation_layers = true;
  c.enable_sync_validation = true;
  if (!g_recon_override.empty()) { c.reconstruct_spv_path = g_recon_override; c.reconstruct_fp16_spv_path = g_recon_override; c.force_fp32 = true; }
  if (!g_sharp_override.empty()) c.sharpen_spv_path = g_sharp_override;
  return c;
}

SsrInitConfig cpu_cfg_for(const gpu::VulkanHarnessConfig& g) {
  SsrInitConfig c;
  c.enable_sharpening = true;
  c.sharpness = g.sharpness;
  c.disocclusion_depth_threshold = g.disocclusion_depth_threshold;
  return c;
}

// ---- a small deterministic scene (analytic, so a ground truth exists at any resolution) --------------------------------------
struct Frame {
  std::vector<ColorRgb> color;
  std::vector<float> depth;
  std::vector<Vec2f> mv;
  std::vector<float> reactive, transp;
  SsrDispatchInputs in{};
};

struct SceneOpts {
  float hdr_peak = 40.0f;      // luminance of the bright blob
  bool moving = true;
  bool glass = false;          // adds a transparency-mask band
  bool reactive = false;
  // Edge softness in output pixels. Gemini's parity tolerance (35 dB) is justified by the GPU's bilinear sampling versus the CPU's Catmull-Rom;
  // that difference is small on band-limited content and large on hard edges, so the strict parity tests use soft edges and the hard-edge
  // number is reported as a finding (print_findings).
  float edge_px = 6.0f;
  float pan = 0.0f;           // whole-scene horizontal pan in output px per frame (camera motion)
  float block_speed = 1.5f;   // output px per frame
  bool flicker = false;       // content inside the glass / reactive rectangles alternates strongly every frame
};

float sstep(float a, float b, float x) { const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }
float box_mask(float x, float lo, float hi, float w) { return sstep(lo - w, lo + w, x) * (1.0f - sstep(hi - w, hi + w, x)); }
ColorRgb mix_c(ColorRgb a, ColorRgb b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

// Scene in output-pixel coordinates: a soft gradient, a thin line, a checker patch, a bright blob, a moving block.
ColorRgb scene_at(float x_in, float y, std::uint32_t frame, const SceneOpts& o) {
  const float x = x_in + o.pan * static_cast<float>(frame);
  const float w = std::max(0.01f, o.edge_px);
  const float u = x / 160.0f, v = y / 90.0f;
  ColorRgb c{0.15f + 0.25f * u, 0.12f + 0.2f * v, 0.2f};
  const float d = std::fabs(y - (30.0f + 0.25f * x));
  c = mix_c(c, {0.9f, 0.9f, 0.1f}, std::clamp(1.0f - d / (0.6f + w), 0.0f, 1.0f));                            // thin line
  const float patch = box_mask(x, 100, 130, w) * box_mask(y, 50, 80, w);
  const float q = o.edge_px < 0.5f ? static_cast<float>((static_cast<int>(x) / 2 + static_cast<int>(y) / 2) & 1) : 0.5f + 0.5f * std::sin(x * 1.1f) * std::sin(y * 1.1f);
  c = mix_c(c, mix_c({0.05f, 0.05f, 0.05f}, {0.8f, 0.8f, 0.8f}, q), patch);                                    // checker
  const float dx = x - 40.0f, dy = y - 60.0f;
  c = mix_c(c, {o.hdr_peak, o.hdr_peak * 0.8f, o.hdr_peak * 0.5f}, 1.0f - sstep(6.0f - w, 6.0f + w, std::sqrt(dx * dx + dy * dy)));  // HDR blob
  const float bx = 60.0f + (o.moving ? o.block_speed * static_cast<float>(frame) : 0.0f);
  c = mix_c(c, {0.1f, 0.6f, 0.2f}, box_mask(x, bx, bx + 14, w) * box_mask(y, 20, 40, w));                      // moving block
  if (o.flicker) {  // rectangles in screen space (before the pan): alternating content that history must NOT trust
    const float k = (frame & 1u) ? 1.8f : 0.35f;
    const float g = box_mask(x_in, 20, 50, w) * box_mask(y, 10, 30, w) + box_mask(x_in, 90, 110, w) * box_mask(y, 10, 25, w);
    c = mix_c(c, {c.r * k, c.g * k, c.b * k}, std::min(1.0f, g));
  }
  return c;
}

Frame make_frame(std::uint32_t frame, std::uint32_t ow, std::uint32_t oh, SsrQualityMode mode, float exposure, bool cut, const SceneOpts& o, bool zero_jitter = false) {
  Frame f;
  const SsrResolution r = SsrContext::compute_render_resolution(ow, oh, mode);
  const Vec2f j = zero_jitter ? Vec2f{0.0f, 0.0f} : SsrContext::halton_jitter_px(frame);
  const std::size_t n = static_cast<std::size_t>(r.width) * r.height;
  f.color.resize(n); f.depth.resize(n); f.mv.resize(n); f.reactive.assign(n, 0.0f); f.transp.assign(n, 0.0f);
  const float sx = static_cast<float>(ow) / static_cast<float>(r.width), sy = static_cast<float>(oh) / static_cast<float>(r.height);
  for (std::uint32_t y = 0; y < r.height; ++y) {
    for (std::uint32_t x = 0; x < r.width; ++x) {
      const std::size_t i = static_cast<std::size_t>(y) * r.width + x;
      const float ox = (static_cast<float>(x) + 0.5f + j.x) * sx, oy = (static_cast<float>(y) + 0.5f + j.y) * sy;
      ColorRgb c = scene_at(ox, oy, frame, o);
      c.r *= exposure; c.g *= exposure; c.b *= exposure;
      f.color[i] = c;
      const float sxp = ox + o.pan * static_cast<float>(frame);  // scene-space x
      const float bxs = 60.0f + o.block_speed * static_cast<float>(frame);
      const bool block = o.moving && sxp > bxs && sxp < bxs + 14.0f && oy > 20 && oy < 40;
      f.depth[i] = block ? 0.7f : 0.2f;
      // content moves right by (block_speed - pan) for the block and by -pan for the background, per frame; mv = prev - curr in render pixels
      f.mv[i] = block ? Vec2f{(o.pan - o.block_speed) / sx, 0.0f} : Vec2f{o.pan / sx, 0.0f};
      if ((o.glass || o.flicker) && ox > 20 && ox < 50 && oy > 10 && oy < 30) f.transp[i] = 0.8f;
      if ((o.reactive || o.flicker) && ox > 90 && ox < 110 && oy > 10 && oy < 25) f.reactive[i] = 1.0f;
    }
  }
  f.in = SsrDispatchInputs{r.width, r.height, ow, oh, f.color.data(), f.depth.data(), f.mv.data(), f.reactive.data(), f.transp.data(), j, exposure, cut, true};
  return f;
}

double psnr_srgb(const std::vector<ColorRgb>& a, const std::vector<ColorRgb>& b) {
  auto tm = [](float v) { const float m = v / (1.0f + std::max(0.0f, v)); return std::pow(std::clamp(m, 0.0f, 1.0f), 1.0f / 2.2f); };
  double mse = 0; const std::size_t n = std::min(a.size(), b.size());
  for (std::size_t i = 0; i < n; ++i) {
    const double dr = static_cast<double>(tm(a[i].r) - tm(b[i].r)), dg = static_cast<double>(tm(a[i].g) - tm(b[i].g)), db = static_cast<double>(tm(a[i].b) - tm(b[i].b));
    mse += (dr * dr + dg * dg + db * db) / 3.0;
  }
  mse /= static_cast<double>(n);
  return mse < 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
}

std::vector<ColorRgb> ground_truth(std::uint32_t ow, std::uint32_t oh, std::uint32_t frame, float exposure, const SceneOpts& o) {
  std::vector<ColorRgb> gt(static_cast<std::size_t>(ow) * oh);
  for (std::uint32_t y = 0; y < oh; ++y)
    for (std::uint32_t x = 0; x < ow; ++x) {
      ColorRgb acc{};
      for (int sy = 0; sy < 4; ++sy)
        for (int sx = 0; sx < 4; ++sx) {
          ColorRgb c = scene_at(static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / 4.0f, static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / 4.0f, frame, o);
          acc.r += c.r; acc.g += c.g; acc.b += c.b;
        }
      gt[static_cast<std::size_t>(y) * ow + x] = {acc.r * exposure / 16.0f, acc.g * exposure / 16.0f, acc.b * exposure / 16.0f};
    }
  return gt;
}

bool all_finite(const std::vector<ColorRgb>& v) {
  for (const ColorRgb& c : v) if (!std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b)) return false;
  return true;
}

struct ParityStats { double worst_psnr = 99.0, worst_mae = 0.0, worst_roi_mae = 0.0, worst_roi_rel = 0.0, cut_psnr = 99.0; int frames = 0; bool all_clean = true; bool all_passed = true; std::string last_status; };

// Region-of-interest comparison against the CPU reference. The whole-frame PSNR/MAE of the harness is dominated by the flat background, and the
// tone-mapped sRGB metric saturates above luminance ~10, so neither can see most temporal-logic or HDR-range errors. The ROIs are the moving block
// and its trail, the glass and reactive rectangles and the HDR blob; the HDR measure is the relative error in LINEAR luminance.
void roi_metrics(const std::vector<ColorRgb>& ref, const std::vector<ColorRgb>& gpu, std::uint32_t ow, std::uint32_t oh, std::uint32_t frame, const SceneOpts& o,
                 double& roi_mae, double& roi_rel) {
  auto tm = [](float v) { const float m = v / (1.0f + std::max(0.0f, v)); return std::pow(std::clamp(m, 0.0f, 1.0f), 1.0f / 2.2f); };
  const float bx = 60.0f + o.block_speed * static_cast<float>(frame) - o.pan * static_cast<float>(frame);
  const float rects[5][4] = {{bx - 14.0f, 15.0f, bx + 30.0f, 45.0f}, {20, 10, 50, 30}, {90, 10, 110, 25}, {30, 50, 50, 70}, {0, 0, 0, 0}};
  double sum = 0, rel = 0; std::size_t n = 0, nr = 0;
  for (std::uint32_t y = 0; y < oh; ++y)
    for (std::uint32_t x = 0; x < ow; ++x) {
      bool in = false;
      for (const auto& r : rects) in = in || (static_cast<float>(x) >= r[0] && static_cast<float>(x) < r[2] && static_cast<float>(y) >= r[1] && static_cast<float>(y) < r[3]);
      if (!in) continue;
      const ColorRgb a = ref[static_cast<std::size_t>(y) * ow + x], b = gpu[static_cast<std::size_t>(y) * ow + x];
      sum += (std::fabs(static_cast<double>(tm(a.r) - tm(b.r))) + std::fabs(static_cast<double>(tm(a.g) - tm(b.g))) + std::fabs(static_cast<double>(tm(a.b) - tm(b.b)))) / 3.0; ++n;
      const double la = 0.2126 * static_cast<double>(a.r) + 0.7152 * static_cast<double>(a.g) + 0.0722 * static_cast<double>(a.b);
      const double lb = 0.2126 * static_cast<double>(b.r) + 0.7152 * static_cast<double>(b.g) + 0.0722 * static_cast<double>(b.b);
      if (la > 0.5) { rel += std::fabs(lb - la) / la; ++nr; }
    }
  roi_mae = n ? sum / static_cast<double>(n) : 0.0;
  roi_rel = nr ? rel / static_cast<double>(nr) : 0.0;
}

// The shared multi-frame sequence: jitter, dynamic resolution, a camera cut, exposure changes, transparency and reactive masks.
// tight = native resolution and zero jitter: the GPU's bilinear and the CPU's Catmull-Rom both evaluate at texel centres, so the kernel difference
// that sets the 35 dB noise floor of the scaled sequences disappears and the comparison can be far tighter.
ParityStats run_sequence(gpu::SsrVulkanHarness& h, const gpu::VulkanHarnessConfig& cfg, bool verbose, float edge_px = 6.0f, bool stress = true, bool tight = false) {
  ParityStats st;
  SsrContext cpu(cpu_cfg_for(cfg));
  std::vector<ColorRgb> cpu_out, gpu_out;
  struct Step { SsrQualityMode mode; float exposure; bool cut; bool glass; bool reactive; };
  const Step steps[] = {
      {SsrQualityMode::Quality, 1.0f, false, false, false}, {SsrQualityMode::Quality, 1.0f, false, true, false},
      {SsrQualityMode::Quality, 1.0f, false, false, true},  {SsrQualityMode::Performance, 1.0f, false, false, false},  // dynamic resolution
      {SsrQualityMode::Performance, 1.0f, true, false, false},                                                        // camera cut
      {SsrQualityMode::Performance, 2.0f, false, false, false}, {SsrQualityMode::Performance, 0.5f, false, true, true},   // exposure steps
      {SsrQualityMode::Balanced, 0.5f, false, false, false}, {SsrQualityMode::Balanced, 0.5f, false, false, false},
  };
  std::uint32_t k = 0;
  for (const Step& s : steps) {
    SceneOpts o; o.glass = s.glass; o.reactive = s.reactive; o.edge_px = edge_px;
    if (stress) { o.pan = 2.5f; o.block_speed = 6.0f; o.flicker = true; }
    const Frame f = make_frame(k, 160, 90, tight ? SsrQualityMode::NativeAA : s.mode, s.exposure, s.cut, o, tight);
    const SsrDispatchMetrics m = cpu.reconstruct(f.in, cpu_out);
    CHECK(m.valid);
    const gpu::VulkanParityReport r = h.execute_frame(f.in, cpu_out, gpu_out);
    st.last_status = r.status_message;
    if (verbose) std::printf("  frame %u: psnr %.2f dB  mae %.5f  max %.4f  clean=%d  %s\n", k, r.cpu_vs_gpu_psnr_db, r.cpu_vs_gpu_mae, r.cpu_vs_gpu_max_abs_err, r.validation_clean, r.status_message.c_str());
    if (!r.executed || !r.parity_evaluated) { st.all_passed = false; st.all_clean = false; break; }
    st.frames++;
    if (tight && (k == 0 || s.cut)) st.cut_psnr = std::min(st.cut_psnr, r.cpu_vs_gpu_psnr_db);
    st.worst_psnr = std::min(st.worst_psnr, r.cpu_vs_gpu_psnr_db);
    st.worst_mae = std::max(st.worst_mae, r.cpu_vs_gpu_mae);
    double rm = 0, rr = 0; roi_metrics(cpu_out, gpu_out, 160, 90, k, o, rm, rr);
    st.worst_roi_mae = std::max(st.worst_roi_mae, rm); st.worst_roi_rel = std::max(st.worst_roi_rel, rr);
    if (verbose) std::printf("           roi mae %.5f  roi HDR rel err %.4f\n", rm, rr);
    st.all_clean = st.all_clean && r.validation_clean;
    st.all_passed = st.all_passed && r.parity_passed;
    if (!r.validation_clean && !r.first_validation_message.empty()) std::fprintf(stderr, "validation: %s\n", r.first_validation_message.c_str());
    ++k;
  }
  return st;
}

// ---- tests ---------------------------------------------------------------------------------------------------------------------
bool test_init_report(const gpu::VulkanHarnessConfig& cfg) {
  gpu::SsrVulkanHarness h; std::string status;
  if (!h.initialize(cfg, status)) { std::printf("no usable Vulkan device or validation layer: %s\n", status.c_str()); return false; }
  std::printf("init: %s\n", status.c_str());
  SceneOpts o; const Frame f = make_frame(0, 160, 90, SsrQualityMode::Quality, 1.0f, false, o);
  std::vector<ColorRgb> out; SsrContext cpu(cpu_cfg_for(cfg)); std::vector<ColorRgb> ref; (void)cpu.reconstruct(f.in, ref);
  const auto r = h.execute_frame(f.in, ref, out);
  std::printf("device '%s' software=%d api=%u.%u fp16=%d validation=%d sync=%d timestamps=%d gpu_ms(recon %.3f sharpen %.3f; NOT performance evidence)\n",
              r.device_name.c_str(), r.is_software_device, r.api_version >> 22, (r.api_version >> 12) & 0x3ff, r.fp16_active, r.validation_active,
              r.sync_validation_active, r.timestamps_valid, r.reconstruct_gpu_ms, r.sharpen_gpu_ms);
  CHECK(r.executed); CHECK(r.validation_active); CHECK(r.validation_clean); CHECK(r.parity_passed);
  CHECK(r.api_version >= ((1u << 22) | (3u << 12)));
  CHECK(out.size() == 160u * 90u);
  CHECK(all_finite(out));
  if (r.timestamps_valid) CHECK(std::isfinite(r.gpu_time_ms) && r.gpu_time_ms >= 0.0);
  return true;
}

void test_parity_sequence(bool fp16) {
  gpu::VulkanHarnessConfig cfg = base_cfg();
  cfg.prefer_fp16 = fp16; cfg.force_fp32 = !fp16;
  gpu::SsrVulkanHarness h; std::string status;
  if (!h.initialize(cfg, status)) { std::printf("[%s] skipped: %s\n", fp16 ? "FP16" : "FP32", status.c_str()); return; }
  std::printf("[%s sequence] %s\n", fp16 ? "FP16" : "FP32", status.c_str());
  const ParityStats st = run_sequence(h, cfg, true);
  std::printf("[%s] frames=%d worst psnr %.2f dB worst mae %.5f clean=%d\n", fp16 ? "FP16" : "FP32", st.frames, st.worst_psnr, st.worst_mae, st.all_clean);
  CHECK(st.frames == 9); CHECK(st.all_clean); CHECK(st.all_passed);
  // The FP16 request only means FP16 when the device offers shaderFloat16; say which path really ran.
  SceneOpts o; const Frame f = make_frame(0, 160, 90, SsrQualityMode::Quality, 1.0f, false, o); std::vector<ColorRgb> out;
  const auto rep = h.execute_frame(f.in, {}, out);
  if (!fp16) CHECK(!rep.fp16_active);
  if (fp16 && !rep.fp16_active) std::printf("[FP16] device does not offer shaderFloat16 (or binary missing): the FP32 path ran; FP16 NOT exercised\n");
}

// ---- directed sequences: one temporal behaviour each, built so that the behaviour changes the output a lot -------------------------------------
// Native resolution, zero jitter (so GPU and CPU agree to rounding on frames without history), textured content so that the history is NOT
// simply pulled onto the current pixel by the neighbourhood clamp. Compared against the CPU reference.
enum class Directed { Disocclusion, Confidence, DepthDilation };

double run_directed(gpu::SsrVulkanHarness& h, const gpu::VulkanHarnessConfig& cfg, Directed which, bool verbose) {
  constexpr std::uint32_t W = 96, H = 64;
  SsrContext cpu(cpu_cfg_for(cfg));
  std::vector<ColorRgb> cpu_out, gpu_out, color(static_cast<std::size_t>(W) * H);
  std::vector<float> depth(color.size()), react(color.size(), 0.0f), transp(color.size(), 0.0f);
  std::vector<Vec2f> mv(color.size());
  double worst = 99.0;
  const int frames = which == Directed::DepthDilation ? 5 : 4;
  for (int k = 0; k < frames; ++k) {
    for (std::uint32_t y = 0; y < H; ++y)
      for (std::uint32_t x = 0; x < W; ++x) {
        const std::size_t i = static_cast<std::size_t>(y) * W + x;
        const bool checker = ((x + y) & 1u) != 0;
        const float v = checker ? 0.85f : 0.06f;
        float d = 0.2f; Vec2f m{0, 0}; ColorRgb c{v, v, v};
        switch (which) {
          case Directed::Disocclusion:   // uniform grey, then a checker at a far greater depth, then back
            if (k == 0 || k == 3) { c = {0.4f, 0.4f, 0.4f}; d = 0.2f; } else { d = 0.7f; }
            break;
          case Directed::Confidence:     // uniform grey, then a checker at the same depth: how fast the history is trusted
            if (k == 0) c = {0.4f, 0.4f, 0.4f};
            break;
          case Directed::DepthDilation: {  // a textured square at depth 0.7 moving 7 px per frame over a textured background at depth 0.2
            const int sq = 20 + 7 * k;
            const bool in = static_cast<int>(x) >= sq && static_cast<int>(x) < sq + 24 && y >= 20 && y < 44;
            if (in) { d = 0.7f; m = {-7.0f, 0.0f}; const float v2 = (((x / 2) + (y / 2)) & 1u) ? 0.95f : 0.15f; c = {v2, v2 * 0.5f, v2}; }
            break;
          }
        }
        color[i] = c; depth[i] = d; mv[i] = m;
      }
    const SsrDispatchInputs in{W, H, W, H, color.data(), depth.data(), mv.data(), react.data(), transp.data(), {0.0f, 0.0f}, 1.0f, false, true};
    (void)cpu.reconstruct(in, cpu_out);
    const auto r = h.execute_frame(in, cpu_out, gpu_out);
    if (!r.executed || !r.parity_evaluated || !r.validation_clean) return 0.0;
    if (verbose) std::printf("    directed %d frame %d: %.2f dB\n", static_cast<int>(which), k, r.cpu_vs_gpu_psnr_db);
    if (k > 0) worst = std::min(worst, r.cpu_vs_gpu_psnr_db);
  }
  return worst;
}
constexpr double kDirectedPsnr = 50.0;  // measured baselines (worst frame with history): disocclusion 76.1, confidence 79.4, depth dilation 58.5 dB (FP16 path); FP32 path worst 52.0 dB

void test_directed_sequences() {
  gpu::VulkanHarnessConfig cfg = base_cfg();
  gpu::SsrVulkanHarness h; std::string status;
  if (!h.initialize(cfg, status)) return;
  for (const Directed d : {Directed::Disocclusion, Directed::Confidence, Directed::DepthDilation}) {
    h.reset_history();
    const double w = run_directed(h, cfg, d, true);
    std::printf("directed %d: worst frame with history %.2f dB (gate >= %.0f)\n", static_cast<int>(d), w, kDirectedPsnr);
    CHECK(w >= kDirectedPsnr);
  }
}

void test_strict_gates() {
  gpu::VulkanHarnessConfig cfg = base_cfg();
  gpu::SsrVulkanHarness h; std::string status;
  if (!h.initialize(cfg, status)) return;
  const ParityStats a = run_sequence(h, cfg, false);
  h.reset_history();
  const ParityStats t = run_sequence(h, cfg, false, 6.0f, true, true);
  std::printf("strict gates: scaled roi mae %.5f (<= %.4f); tight psnr %.2f (>= %.1f) mae %.5f (<= %.4f) roi mae %.5f (<= %.4f) cut/first-frame psnr %.2f (>= %.1f)\n",
              a.worst_roi_mae, kScaledRoiMae, t.worst_psnr, kTightPsnr, t.worst_mae, kTightMae, t.worst_roi_mae, kTightRoiMae, t.cut_psnr, kTightCutPsnr);
  CHECK(a.frames == 9 && a.all_clean && a.all_passed && a.worst_roi_mae <= kScaledRoiMae);
  CHECK(t.frames == 9 && t.all_clean && t.worst_psnr >= kTightPsnr && t.worst_mae <= kTightMae && t.worst_roi_mae <= kTightRoiMae && t.cut_psnr >= kTightCutPsnr);
}

void test_reference_negatives() {
  gpu::VulkanHarnessConfig cfg = base_cfg();
  gpu::SsrVulkanHarness h; std::string status;
  if (!h.initialize(cfg, status)) return;
  SceneOpts o; const Frame f = make_frame(0, 160, 90, SsrQualityMode::Quality, 1.0f, false, o);
  SsrContext cpu(cpu_cfg_for(cfg)); std::vector<ColorRgb> good; (void)cpu.reconstruct(f.in, good);
  std::vector<ColorRgb> out;
  const std::size_t n = 160u * 90u;

  // The director's defect: an empty reference used to be reported as PASSED.
  auto r = h.execute_frame(f.in, {}, out);
  CHECK(r.executed); CHECK(!r.parity_evaluated); CHECK(!r.parity_passed); CHECK(!r.reference_valid);
  CHECK(out.size() == n);  // the GPU result is still returned

  std::vector<ColorRgb> shortref(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n / 2));
  r = h.execute_frame(f.in, shortref, out);
  CHECK(!r.parity_evaluated); CHECK(!r.parity_passed);
  std::vector<ColorRgb> longref = good; longref.push_back({});
  r = h.execute_frame(f.in, longref, out);
  CHECK(!r.parity_evaluated); CHECK(!r.parity_passed);

  std::vector<ColorRgb> nanref = good; nanref[n / 3].g = std::numeric_limits<float>::quiet_NaN();
  r = h.execute_frame(f.in, nanref, out);
  CHECK(!r.parity_evaluated); CHECK(!r.parity_passed);
  std::vector<ColorRgb> infref = good; infref[7].r = std::numeric_limits<float>::infinity();
  r = h.execute_frame(f.in, infref, out);
  CHECK(!r.parity_evaluated); CHECK(!r.parity_passed);

  // A reference of the right size and finite but WRONG must fail (all zero, and a shifted copy).
  std::vector<ColorRgb> zero(n, ColorRgb{});
  r = h.execute_frame(f.in, zero, out);
  CHECK(r.parity_evaluated); CHECK(!r.parity_passed);
  std::vector<ColorRgb> bright(n, ColorRgb{50.0f, 50.0f, 50.0f});
  r = h.execute_frame(f.in, bright, out);
  CHECK(r.parity_evaluated); CHECK(!r.parity_passed);
  // The matching reference passes again on the same harness (recovery after the negatives). The negatives ran frames, so recompute.
  SsrContext fresh(cpu_cfg_for(cfg));
  h.reset_history();
  (void)fresh.reconstruct(f.in, good);
  r = h.execute_frame(f.in, good, out);
  CHECK(r.parity_passed); CHECK(r.validation_clean);

  // Invalid dispatch inputs are rejected without touching the GPU state, and the next valid frame still works.
  r = h.execute_frame(SsrDispatchInputs{}, good, out);
  CHECK(!r.executed);
  SsrDispatchInputs bad = f.in; bad.render_width = bad.output_width + 1;
  r = h.execute_frame(bad, good, out); CHECK(!r.executed);
  bad = f.in; bad.depth = nullptr;
  r = h.execute_frame(bad, good, out); CHECK(!r.executed);
  bad = f.in; bad.output_width = 1u << 20;  // beyond maxImageDimension2D: refused, not a crash
  bad.render_width = 100;
  r = h.execute_frame(bad, good, out); CHECK(!r.executed);
  h.reset_history();
  SsrContext again(cpu_cfg_for(cfg)); (void)again.reconstruct(f.in, good);
  r = h.execute_frame(f.in, good, out);
  CHECK(r.executed); CHECK(r.parity_passed); CHECK(r.validation_clean);
}

void test_history_persistence_and_accuracy() {
  gpu::VulkanHarnessConfig cfg = base_cfg();
  gpu::SsrVulkanHarness h; std::string status;
  if (!h.initialize(cfg, status)) return;
  SceneOpts o; o.moving = false; o.edge_px = 1.5f;  // detail: thin line, sharp checker
  std::vector<ColorRgb> out, first, bil;
  double psnr_first = 0, psnr_last = 0, psnr_bilinear = 0;
  const std::uint32_t N = 24;
  for (std::uint32_t k = 0; k < N; ++k) {
    const Frame f = make_frame(k, 160, 90, SsrQualityMode::Performance, 1.0f, false, o);
    const auto r = h.execute_frame(f.in, {}, out);
    CHECK(r.executed); CHECK(r.validation_clean);
    const auto gt = ground_truth(160, 90, k, 1.0f, o);
    const double p = psnr_srgb(out, gt);
    if (k == 0) { psnr_first = p; first = out; upscale_bilinear(f.in.color_hdr, f.in.render_width, f.in.render_height, 160, 90, bil); psnr_bilinear = psnr_srgb(bil, gt); }
    psnr_last = p;
  }
  std::printf("history: static scene, 24 jittered frames at 50%%: GPU vs supersampled truth: frame 1 %.2f dB, frame 24 %.2f dB; single-frame bilinear %.2f dB\n", psnr_first, psnr_last, psnr_bilinear);
  CHECK_MSG(psnr_last > psnr_first + 0.25, "temporal accumulation did not improve the output (%.2f -> %.2f)", psnr_first, psnr_last);
  CHECK_MSG(psnr_last > psnr_bilinear, "converged GPU output is not better than bilinear (%.2f vs %.2f)", psnr_last, psnr_bilinear);
  // The history is persistent: the same frame with the history reset (a cut) is NOT what the accumulated frame produced.
  const Frame f = make_frame(N, 160, 90, SsrQualityMode::Performance, 1.0f, false, o);
  std::vector<ColorRgb> with_hist, no_hist;
  (void)h.execute_frame(f.in, {}, with_hist);
  h.reset_history();
  (void)h.execute_frame(f.in, {}, no_hist);
  double diff = 0; for (std::size_t i = 0; i < with_hist.size(); ++i) diff += static_cast<double>(std::fabs(with_hist[i].r - no_hist[i].r));
  std::printf("history: sum |with - without history| = %.3f\n", diff);
  CHECK(diff > 1.0);
}

void test_invalid_inputs_and_hdr() {
  gpu::VulkanHarnessConfig cfg = base_cfg();
  gpu::SsrVulkanHarness h; std::string status;
  if (!h.initialize(cfg, status)) return;
  SsrContext cpu(cpu_cfg_for(cfg)); std::vector<ColorRgb> ref, out;
  SceneOpts o;
  const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
  for (std::uint32_t k = 0; k < 4; ++k) {
    Frame f = make_frame(k, 160, 90, SsrQualityMode::Performance, 1.0f, false, o);
    if (k >= 1) {  // poison the inputs
      f.color[10] = {nan, 1, 1}; f.color[11] = {inf, 1, 1}; f.color[12] = {-5, -5, -5}; f.color[13] = {1e30f, 1e30f, 1e30f};
      f.depth[20] = nan; f.depth[21] = inf; f.depth[22] = -3.0f; f.depth[23] = 7.0f;
      f.mv[30] = {nan, 0}; f.mv[31] = {inf, -inf}; f.mv[32] = {1e30f, -1e30f};
      f.reactive[40] = nan; f.transp[41] = 9.0f;
    }
    if (k == 2) f.in.pre_exposure = nan;
    if (k == 3) f.in.pre_exposure = 0.0f;
    (void)cpu.reconstruct(f.in, ref);
    const auto r = h.execute_frame(f.in, ref, out);
    CHECK_MSG(r.executed, "frame %u: %s", k, r.status_message.c_str());
    CHECK_MSG(all_finite(out), "frame %u: non-finite GPU output", k);
    CHECK_MSG(r.validation_clean, "frame %u: %s", k, r.first_validation_message.c_str());
    CHECK_MSG(r.parity_passed, "frame %u: psnr %.2f mae %.4f", k, r.cpu_vs_gpu_psnr_db, r.cpu_vs_gpu_mae);
  }
  // HDR extremes through the temporal path: finite output, parity with the CPU reference.
  SceneOpts hot; hot.hdr_peak = 3.0e5f; hot.moving = false;
  SsrContext cpu2(cpu_cfg_for(cfg)); h.reset_history();
  for (std::uint32_t k = 0; k < 3; ++k) {
    const Frame f = make_frame(k, 160, 90, SsrQualityMode::Performance, 1.0f, false, hot);
    (void)cpu2.reconstruct(f.in, ref);
    const auto r = h.execute_frame(f.in, ref, out);
    CHECK(r.executed); CHECK(all_finite(out)); CHECK(r.validation_clean);
    // Parity at a 3e5 peak is NOT asserted: the CPU's Catmull-Rom rings around a spike that large and the GPU's bilinear does not (measured in
    // --findings: about 20 to 29 dB). The hard requirements are a finite output and a clean validation run.
    CHECK(r.parity_evaluated);
  }
}

void test_validation_reaches_report() {
  gpu::VulkanHarnessConfig cfg = base_cfg();
  cfg.inject_validation_error = true;
  gpu::SsrVulkanHarness h; std::string status;
  if (!h.initialize(cfg, status)) return;
  SceneOpts o; const Frame f = make_frame(0, 160, 90, SsrQualityMode::Quality, 1.0f, false, o); std::vector<ColorRgb> out;
  const auto r = h.execute_frame(f.in, {}, out);
  CHECK(r.executed); CHECK(r.validation_active);
  CHECK_MSG(!r.validation_clean && r.validation_message_count >= 1, "an injected invalid call was not reported (count %u)", r.validation_message_count);
  std::printf("validation hook: injected error reported = %d (%u message(s))\n", !r.validation_clean, r.validation_message_count);
  // And with the layer off the report must not claim a clean run.
  gpu::VulkanHarnessConfig off = base_cfg(); off.enable_validation_layers = false;
  gpu::SsrVulkanHarness h2; if (!h2.initialize(off, status)) return;
  const auto r2 = h2.execute_frame(f.in, {}, out);
  CHECK(r2.executed); CHECK(!r2.validation_active); CHECK(!r2.validation_clean);
}

void test_lifetime_and_errors() {
  const gpu::VulkanHarnessConfig cfg = base_cfg();
  // Repeated create / run / destroy: under ASan a leak or a double free fails the process; the layer reports any live object at destroy.
  for (int i = 0; i < 6; ++i) {
    gpu::SsrVulkanHarness h; std::string status;
    if (!h.initialize(cfg, status)) return;
    SceneOpts o; const Frame f = make_frame(0, 160, 90, SsrQualityMode::Quality, 1.0f, false, o); std::vector<ColorRgb> out;
    const auto r = h.execute_frame(f.in, {}, out);
    CHECK(r.executed); CHECK(r.validation_clean);
    h.destroy(); h.destroy();  // idempotent
    const auto r2 = h.execute_frame(f.in, {}, out);
    CHECK(!r2.executed);  // after destroy the harness refuses work
  }
  // Initialization failures return false with a status and leave nothing behind.
  { gpu::VulkanHarnessConfig bad = cfg; bad.reconstruct_spv_path = "/nonexistent/none.spv"; bad.reconstruct_fp16_spv_path = bad.reconstruct_spv_path;
    gpu::SsrVulkanHarness h; std::string st; CHECK(!h.initialize(bad, st)); CHECK(!st.empty()); }
  { // a file that is not SPIR-V: the pipeline creation must fail and be reported
    const std::string path = g_shader_dir + "/garbage.spv";
    if (std::FILE* fp = std::fopen(path.c_str(), "wb")) { const std::uint32_t junk[64] = {}; std::fwrite(junk, 4, 64, fp); std::fclose(fp); }
    gpu::VulkanHarnessConfig bad = cfg; bad.reconstruct_spv_path = path; bad.force_fp32 = true;
    gpu::SsrVulkanHarness h; std::string st; CHECK(!h.initialize(bad, st)); CHECK(!st.empty()); std::remove(path.c_str()); }
  { // strict selection of a kind that does not exist must fail rather than fall back
    gpu::VulkanHarnessConfig strict = cfg; strict.strict_device_selection = true; strict.prefer_software_vulkan = false;
    gpu::SsrVulkanHarness h; std::string st; const bool ok = h.initialize(strict, st);
    std::printf("strict hardware selection: %s (%s)\n", ok ? "a hardware device exists" : "refused, as expected on a software-only machine", st.c_str()); }
  // Rapid resolution changes (output and render) with the layer watching: no stale descriptor or layout.
  gpu::SsrVulkanHarness h; std::string status;
  if (!h.initialize(cfg, status)) return;
  const std::uint32_t sizes[][2] = {{160, 90}, {96, 54}, {160, 90}, {200, 120}, {96, 54}};
  SceneOpts o; std::uint32_t k = 0; std::vector<ColorRgb> out;
  for (const auto& s : sizes) {
    const Frame f = make_frame(k++, s[0], s[1], SsrQualityMode::Balanced, 1.0f, false, o);
    const auto r = h.execute_frame(f.in, {}, out);
    CHECK(r.executed); CHECK(r.validation_clean); CHECK(out.size() == static_cast<std::size_t>(s[0]) * s[1]);
  }
}

// Algorithm findings, measured and printed (reported to Gemini through the director). They never fail the build.
void print_findings() {
  gpu::VulkanHarnessConfig cfg = base_cfg();
  gpu::SsrVulkanHarness h; std::string status;
  if (!h.initialize(cfg, status)) return;
  std::printf("\nFINDINGS (measured on '%s'; software Vulkan is not performance evidence)\n", status.c_str());
  // F1: HDR luminance after the temporal path (the decompress floor 1e-3 caps luminance near 1000), and the cut frame vs the next frame.
  for (const float peak : {50.0f, 500.0f, 5000.0f, 50000.0f, 500000.0f}) {
    h.reset_history();
    SceneOpts o; o.hdr_peak = peak; o.moving = false; std::vector<ColorRgb> out0, out1;
    const Frame f0 = make_frame(0, 160, 90, SsrQualityMode::Performance, 1.0f, false, o);
    const Frame f1 = make_frame(1, 160, 90, SsrQualityMode::Performance, 1.0f, false, o);
    (void)h.execute_frame(f0.in, {}, out0); (void)h.execute_frame(f1.in, {}, out1);
    auto peak_of = [](const std::vector<ColorRgb>& v) { float m = 0; for (const ColorRgb& c : v) m = std::max(m, 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b); return m; };
    std::printf("  HDR blob luminance %.0f: output peak frame 0 (history cut) %.1f, frame 1 (history used) %.1f\n", static_cast<double>(peak * 0.8f), static_cast<double>(peak_of(out0)), static_cast<double>(peak_of(out1)));
  }
  // F0: the 35 dB parity tolerance is a statement about band-limited content: the GPU samples the current colour with hardware bilinear
  // filtering, the CPU reference uses Catmull-Rom. On sharp edges the two diverge by design.
  for (const float e : {6.0f, 2.0f, 0.01f}) {
    h.reset_history();
    const ParityStats s = run_sequence(h, cfg, false, e);
    std::printf("  CPU-vs-GPU parity, 9-frame sequence, edge softness %.2f px: worst %.2f dB, worst MAE %.4f (threshold 35 dB / 0.025)\n", static_cast<double>(e), s.worst_psnr, s.worst_mae);
  }
  {  // F1b: parity on the 3e5 HDR scene (informational)
    SceneOpts hot; hot.hdr_peak = 3.0e5f; hot.moving = false; SsrContext c(cpu_cfg_for(cfg)); std::vector<ColorRgb> ref, out; h.reset_history();
    for (std::uint32_t k = 0; k < 3; ++k) { const Frame f = make_frame(k, 160, 90, SsrQualityMode::Performance, 1.0f, false, hot); (void)c.reconstruct(f.in, ref); const auto r = h.execute_frame(f.in, ref, out);
      std::printf("  HDR 3e5 blob, frame %u: CPU-vs-GPU %.2f dB (the CPU reference has the same 1000-luminance ceiling, so parity cannot see that defect)\n", k, r.cpu_vs_gpu_psnr_db); }
  }
  // F2: disocclusion threshold in raw depth: far surfaces (reversed-Z depth ~1e-4) can never exceed an absolute 0.05 difference.
  std::printf("  disocclusion: the test is |center_depth - prev_depth| > %.2f in raw reversed-Z depth; a perspective depth buffer puts everything beyond a few metres below 0.01, so far-field disocclusions are never detected\n", static_cast<double>(cfg.disocclusion_depth_threshold));
  std::printf("  history depth is sampled with a linear sampler (u_history_depth_in), which blends depths across silhouettes\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 4 && std::strcmp(argv[1], "--mutant") == 0) {
    g_recon_override = argv[2]; g_sharp_override = argv[3];
    const gpu::VulkanHarnessConfig cfg = base_cfg();
    gpu::SsrVulkanHarness h; std::string st;
    if (!h.initialize(cfg, st)) { std::printf("mutant: no usable device or layer (%s): skipped\n", st.c_str()); return 77; }
    const ParityStats a = run_sequence(h, cfg, false);
    h.reset_history();
    const ParityStats t = run_sequence(h, cfg, false, 6.0f, true, true);
    double dw = 99.0;
    for (const Directed d : {Directed::Disocclusion, Directed::Confidence, Directed::DepthDilation}) { h.reset_history(); dw = std::min(dw, run_directed(h, cfg, d, false)); }
    const bool scaled_ok = a.frames == 9 && a.all_clean && a.all_passed && a.worst_roi_mae <= kScaledRoiMae;
    const bool directed_ok = dw >= kDirectedPsnr;
    const bool tight_ok = directed_ok && t.frames == 9 && t.all_clean && t.worst_psnr >= kTightPsnr && t.worst_mae <= kTightMae && t.worst_roi_mae <= kTightRoiMae && t.cut_psnr >= kTightCutPsnr;
    std::printf("mutant: scaled(roi mae %.5f psnr %.2f)%s tight(psnr %.2f mae %.5f roi %.5f cut %.2f) directed %.2f%s -> %s\n", a.worst_roi_mae, a.worst_psnr, scaled_ok ? "" : " FAIL", t.worst_psnr, t.worst_mae, t.worst_roi_mae, t.cut_psnr, dw, tight_ok ? "" : " FAIL", scaled_ok && tight_ok ? "SURVIVED" : "DETECTED");
    return scaled_ok && tight_ok ? 0 : 1;
  }
  if (argc >= 2 && std::strcmp(argv[1], "--tight") == 0) {
    const gpu::VulkanHarnessConfig c = base_cfg(); gpu::SsrVulkanHarness h; std::string st; if (!h.initialize(c, st)) return 77;
    const ParityStats s = run_sequence(h, c, true, 6.0f, true, true);
    std::printf("tight: frames=%d worst psnr %.2f mae %.5f roi mae %.5f roi rel %.4f\n", s.frames, s.worst_psnr, s.worst_mae, s.worst_roi_mae, s.worst_roi_rel); return 0; }
  const bool findings = argc >= 2 && std::strcmp(argv[1], "--findings") == 0;
  const gpu::VulkanHarnessConfig cfg = base_cfg();
  if (!test_init_report(cfg)) return 77;  // no device / layer: reported as skipped, never as a pass
  test_parity_sequence(false);
  test_parity_sequence(true);
  test_strict_gates();
  test_directed_sequences();
  test_reference_negatives();
  test_history_persistence_and_accuracy();
  test_invalid_inputs_and_hdr();
  test_validation_reaches_report();
  test_lifetime_and_errors();
  if (findings) print_findings();
  if (g_failures != 0) { std::fprintf(stderr, "ssr_validation_test: %d FAILED\n", g_failures); return 1; }
  std::printf("ssr_validation_test: ALL PASSED\n");
  return 0;
}
