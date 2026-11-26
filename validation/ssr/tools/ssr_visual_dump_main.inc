// Appended to a copy of Gemini's ssr_test.cpp (so the scenes, the TAA-U baseline and the metrics are the delivered ones; `main` there is renamed
// upstream_main). Produces the independent visual and measurement evidence for the validation checkpoint. CPU timings are CPU; the GPU run is on
// whatever Vulkan device the loader offers (llvmpipe on the hosted lanes): software Vulkan is NOT GPU performance evidence.
//
//   ssr_visual_dump <out_dir> stills   1920x1080 output, Performance (50%), frames after a warm-up: every method's image as PPM
//   ssr_visual_dump <out_dir> clip     150 frames, 960x540 output, 1:1 centre crops of four methods side by side, PPM frames
//   ssr_visual_dump <out_dir> bench    640x360 Performance, 24 frames per scene: PSNR / SSIM / flicker for every method incl. the GPU path, JSON
#include <cstdio>

namespace {
struct Methods {
  std::vector<ColorRgb> gt, bil, bic, taau, cpu, gpu_out;
};

std::string ppm_path(const std::string& dir, const char* scene, const char* method) { return dir + "/" + scene + "_" + method + ".ppm"; }

const char* scene_name(SceneKind k) {
  switch (k) {
    case SceneKind::MovingVehicleAndFence: return "vehicle_fence";
    case SceneKind::FineFoliageAndPowerLines: return "foliage_wires";
    case SceneKind::NighttimeNeonHdr: return "night_hdr";
    case SceneKind::ReflectiveRoadAndGlass: return "road_glass";
    case SceneKind::RapidCameraPanAndCut: return "pan_cut";
  }
  return "?";
}
}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: ssr_visual_dump <out_dir> stills|clip|bench\n"); return 2; }
  const std::string dir = argv[1], mode = argv[2];
  gpu::VulkanHarnessConfig gcfg;
  gcfg.reconstruct_spv_path = std::string(SSR_VALIDATION_SHADER_DIR) + "/ssr_reconstruct.spv";
  gcfg.reconstruct_fp16_spv_path = std::string(SSR_VALIDATION_SHADER_DIR) + "/ssr_reconstruct_fp16.spv";
  gcfg.sharpen_spv_path = std::string(SSR_VALIDATION_SHADER_DIR) + "/ssr_sharpen.spv";
  gcfg.sharpness = 0.25f;  // the CPU reference's default
  gcfg.force_fp32 = true;  // one path for the evidence; the FP16 path is covered by the validation suite
  gpu::SsrVulkanHarness harness;
  std::string status;
  if (!harness.initialize(gcfg, status)) { std::fprintf(stderr, "no Vulkan device / layer: %s\n", status.c_str()); return 77; }
  std::printf("%s\n", status.c_str());

  const SceneKind kinds[] = {SceneKind::MovingVehicleAndFence, SceneKind::FineFoliageAndPowerLines, SceneKind::NighttimeNeonHdr,
                             SceneKind::ReflectiveRoadAndGlass, SceneKind::RapidCameraPanAndCut};
  SsrInitConfig scfg; scfg.sharpness = gcfg.sharpness; scfg.disocclusion_depth_threshold = gcfg.disocclusion_depth_threshold;

  if (mode == "stills") {
    constexpr std::uint32_t W = 1920, H = 1080, WARM = 10;
    const SsrResolution ren = SsrContext::compute_render_resolution(W, H, SsrQualityMode::Performance);
    for (const SceneKind kind : kinds) {
      SsrContext cpu(scfg); TaauBaselineContext taau; harness.reset_history();
      Methods m; std::vector<ColorRgb> gpu_tmp;
      double gpu_ms = 0, par = 0;
      for (std::uint32_t f = 0; f <= WARM; ++f) {
        const Vec2f j = SsrContext::halton_jitter_px(f);
        const auto sf = render_scene(kind, W, H, ren.width, ren.height, f, j, 1.0f);
        SsrDispatchInputs in{ren.width, ren.height, W, H, sf.low_color.data(), sf.low_depth.data(), sf.low_mv.data(), sf.low_reactive.data(), sf.low_transp.data(), j, 1.0f, f == 0, true};
        (void)cpu.reconstruct(in, m.cpu);
        const auto rep = harness.execute_frame(in, m.cpu, m.gpu_out);
        taau.run(in, m.taau);
        if (f == WARM) {
          m.gt = sf.gt_color;
          upscale_bilinear(sf.low_color.data(), ren.width, ren.height, W, H, m.bil);
          upscale_bicubic(sf.low_color.data(), ren.width, ren.height, W, H, m.bic);
          gpu_ms = rep.gpu_time_ms; par = rep.cpu_vs_gpu_psnr_db;
          std::printf("%s frame %u: GPU-vs-CPU %.2f dB; PSNR vs truth: bilinear %.2f bicubic %.2f TAA-U %.2f SSR-CPU %.2f SSR-GPU %.2f (GPU %.1f ms on '%s', software=%d, NOT performance evidence)\n", scene_name(kind), f, par,
                      compute_psnr_db(m.gt, m.bil), compute_psnr_db(m.gt, m.bic), compute_psnr_db(m.gt, m.taau), compute_psnr_db(m.gt, m.cpu), compute_psnr_db(m.gt, m.gpu_out), gpu_ms, rep.device_name.c_str(), rep.is_software_device);
        }
      }
      write_ppm(ppm_path(dir, scene_name(kind), "gt"), m.gt, W, H);
      write_ppm(ppm_path(dir, scene_name(kind), "bilinear"), m.bil, W, H);
      write_ppm(ppm_path(dir, scene_name(kind), "bicubic"), m.bic, W, H);
      write_ppm(ppm_path(dir, scene_name(kind), "taau"), m.taau, W, H);
      write_ppm(ppm_path(dir, scene_name(kind), "ssr_cpu"), m.cpu, W, H);
      write_ppm(ppm_path(dir, scene_name(kind), "ssr_gpu"), m.gpu_out, W, H);
    }
    return 0;
  }

  if (mode == "clip") {
    constexpr std::uint32_t W = 960, H = 540, CW = 640, CH = 360, X0 = (W - CW) / 2, Y0 = (H - CH) / 2;
    const SsrResolution ren = SsrContext::compute_render_resolution(W, H, SsrQualityMode::Performance);
    SsrContext cpu(scfg); TaauBaselineContext taau; harness.reset_history();
    std::vector<ColorRgb> cpu_out, gpu_out, taau_out, quad(1280u * 720u);
    for (std::uint32_t f = 0; f < 150; ++f) {
      const Vec2f j = SsrContext::halton_jitter_px(f);
      const SceneKind kind = f < 75 ? SceneKind::MovingVehicleAndFence : SceneKind::NighttimeNeonHdr;
      const auto sf = render_scene(kind, W, H, ren.width, ren.height, f, j, 1.0f);
      SsrDispatchInputs in{ren.width, ren.height, W, H, sf.low_color.data(), sf.low_depth.data(), sf.low_mv.data(), sf.low_reactive.data(), sf.low_transp.data(), j, 1.0f, f == 75 || f == 0, true};
      (void)cpu.reconstruct(in, cpu_out);
      const auto rep = harness.execute_frame(in, cpu_out, gpu_out);
      if (!rep.executed) { std::fprintf(stderr, "GPU frame %u failed: %s\n", f, rep.status_message.c_str()); return 1; }
      taau.run(in, taau_out);
      auto blit = [&](const std::vector<ColorRgb>& src, std::uint32_t qx, std::uint32_t qy, bool diff) {
        for (std::uint32_t y = 0; y < CH; ++y)
          for (std::uint32_t x = 0; x < CW; ++x) {
            const std::size_t si = static_cast<std::size_t>(Y0 + y) * W + (X0 + x);
            ColorRgb c = src[si];
            if (diff) {  // |GPU - truth| in tone-mapped sRGB, x6
              const ColorRgb a = tonemap_srgb(src[si]), b = tonemap_srgb(sf.gt_color[si]);
              c = {std::min(1.0f, std::fabs(a.r - b.r) * 6.0f), std::min(1.0f, std::fabs(a.g - b.g) * 6.0f), std::min(1.0f, std::fabs(a.b - b.b) * 6.0f)};
              // The writer tone-maps linear values; invert that so the difference is shown as displayed: v = m / (1 - m) with m = d^2.2.
              auto inv = [](float d) { const float m = std::min(0.999f, std::pow(d, 2.2f)); return m / (1.0f - m); };
              c = {inv(c.r), inv(c.g), inv(c.b)};
            }
            quad[static_cast<std::size_t>(qy + y) * 1280 + (qx + x)] = c;
          }
      };
      blit(sf.gt_color, 0, 0, false);          // top-left: ground truth
      blit(taau_out, 640, 0, false);           // top-right: conventional TAA-U
      blit(gpu_out, 0, 360, false);            // bottom-left: SSR on the GPU path
      blit(gpu_out, 640, 360, true);           // bottom-right: |SSR(GPU) - truth| x6
      char buf[64]; std::snprintf(buf, sizeof(buf), "/clip_%04u.ppm", f);
      write_ppm(dir + buf, quad, 1280, 720);
    }
    return 0;
  }

  if (mode == "bench") {
    constexpr std::uint32_t W = 640, H = 360, N = 24;
    const SsrResolution ren = SsrContext::compute_render_resolution(W, H, SsrQualityMode::Performance);
    std::printf("{\n  \"output\": \"640x360\", \"mode\": \"Performance (50%%)\", \"frames\": %u, \"scenes\": [\n", N);
    bool first = true;
    for (const SceneKind kind : kinds) {
      SsrContext cpu(scfg); TaauBaselineContext taau; harness.reset_history();
      std::vector<ColorRgb> cpu_out, gpu_out, taau_out, bil, bic, prev_gt, prev_cpu, prev_gpu, prev_taau, prev_bil;
      double p[5] = {}, sm[5] = {}, fl[3] = {}; double par = 99.0; int nf = 0; double cpu_ms = 0, gpu_ms = 0; int gms = 0;
      for (std::uint32_t f = 0; f < N; ++f) {
        const Vec2f j = SsrContext::halton_jitter_px(f);
        const auto sf = render_scene(kind, W, H, ren.width, ren.height, f, j, 1.0f);
        SsrDispatchInputs in{ren.width, ren.height, W, H, sf.low_color.data(), sf.low_depth.data(), sf.low_mv.data(), sf.low_reactive.data(), sf.low_transp.data(), j, 1.0f, (kind == SceneKind::RapidCameraPanAndCut && f == 12) || f == 0, true};
        const auto m = cpu.reconstruct(in, cpu_out);
        const auto rep = harness.execute_frame(in, cpu_out, gpu_out);
        if (!rep.executed) { std::fprintf(stderr, "GPU frame failed: %s\n", rep.status_message.c_str()); return 1; }
        taau.run(in, taau_out);
        upscale_bilinear(sf.low_color.data(), ren.width, ren.height, W, H, bil);
        upscale_bicubic(sf.low_color.data(), ren.width, ren.height, W, H, bic);
        if (f >= 4) {
          p[0] += compute_psnr_db(sf.gt_color, bil); p[1] += compute_psnr_db(sf.gt_color, bic); p[2] += compute_psnr_db(sf.gt_color, taau_out);
          p[3] += compute_psnr_db(sf.gt_color, cpu_out); p[4] += compute_psnr_db(sf.gt_color, gpu_out);
          sm[0] += compute_ssim(sf.gt_color, bil, W, H); sm[1] += compute_ssim(sf.gt_color, bic, W, H); sm[2] += compute_ssim(sf.gt_color, taau_out, W, H);
          sm[3] += compute_ssim(sf.gt_color, cpu_out, W, H); sm[4] += compute_ssim(sf.gt_color, gpu_out, W, H);
          fl[0] += compute_temporal_flicker_rmse(prev_taau, taau_out, prev_gt, sf.gt_color);
          fl[1] += compute_temporal_flicker_rmse(prev_cpu, cpu_out, prev_gt, sf.gt_color);
          fl[2] += compute_temporal_flicker_rmse(prev_gpu, gpu_out, prev_gt, sf.gt_color);
          par = std::min(par, rep.cpu_vs_gpu_psnr_db);
          cpu_ms += m.cpu_time_ms; if (rep.timestamps_valid) { gpu_ms += rep.gpu_time_ms; ++gms; }
          ++nf;
        }
        prev_gt = sf.gt_color; prev_cpu = cpu_out; prev_gpu = gpu_out; prev_taau = taau_out; prev_bil = bil;
      }
      const double d = static_cast<double>(nf);
      std::printf("%s    {\"scene\": \"%s\", \"psnr_db\": {\"bilinear\": %.2f, \"bicubic\": %.2f, \"taau\": %.2f, \"ssr_cpu\": %.2f, \"ssr_gpu\": %.2f}, \"ssim\": {\"bilinear\": %.4f, \"bicubic\": %.4f, \"taau\": %.4f, \"ssr_cpu\": %.4f, \"ssr_gpu\": %.4f}, \"flicker_rmse\": {\"taau\": %.4f, \"ssr_cpu\": %.4f, \"ssr_gpu\": %.4f}, \"gpu_vs_cpu_worst_psnr_db\": %.2f, \"cpu_ms_per_frame\": %.2f, \"gpu_ms_per_frame_software_vulkan\": %.2f}",
                  first ? "" : ",\n", scene_name(kind), p[0] / d, p[1] / d, p[2] / d, p[3] / d, p[4] / d, sm[0] / d, sm[1] / d, sm[2] / d, sm[3] / d, sm[4] / d, fl[0] / d, fl[1] / d, fl[2] / d, par, cpu_ms / d, gms ? gpu_ms / gms : 0.0);
      first = false;
    }
    std::printf("\n  ]\n}\n");
    return 0;
  }
  return upstream_main(argc, argv);
}
