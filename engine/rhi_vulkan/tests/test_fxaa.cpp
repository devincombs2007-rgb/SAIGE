// FXAA on a real (validated) Vulkan device. (1) The pass is compared texel by texel with the CPU reference (`render::fxaa_apply`) on a synthetic image full of aliased edges
// (a disc, diagonal and thin lines, gradients, noise), for a UNORM target (display values) and an sRGB target (linear sampling, perceptual luma). (2) Through the scene renderer, in
// LDR and in HDR: every pixel whose neighbourhood in the unsmoothed image is flat comes out unchanged, the edges are smoothed, and the result is deterministic.
// Software Vulkan (lavapipe in CI).

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/render/fxaa.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 64;
constexpr uint32_t kH = 48;

/// A synthetic RGBA8 image (display values, 0..255) with aliased edges of every kind.
std::vector<uint8_t> make_pixels()
{
    std::vector<uint8_t> px(std::size_t{kW} * kH * 4, 255);
    const auto put = [&](uint32_t x, uint32_t y, float r, float g, float b) {
        uint8_t* p = &px[(std::size_t{y} * kW + x) * 4];
        p[0] = static_cast<uint8_t>(std::lround(std::clamp(r, 0.0f, 1.0f) * 255.0f));
        p[1] = static_cast<uint8_t>(std::lround(std::clamp(g, 0.0f, 1.0f) * 255.0f));
        p[2] = static_cast<uint8_t>(std::lround(std::clamp(b, 0.0f, 1.0f) * 255.0f));
        p[3] = 255;
    };
    uint32_t state = 99u;
    for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
            const float fx = static_cast<float>(x) / kW, fy = static_cast<float>(y) / kH;
            float r = 0.15f + 0.25f * fx, g = 0.2f + 0.2f * fy, b = 0.35f;  // a smooth gradient background
            state = state * 1664525u + 1013904223u;
            const float noise = 0.01f * (static_cast<float>(state >> 8) / 16777216.0f);
            r += noise;
            g += noise;
            b += noise;
            const float dx = static_cast<float>(x) + 0.5f - 22.0f, dy = static_cast<float>(y) + 0.5f - 24.0f;
            if (dx * dx + dy * dy < 14.0f * 14.0f) {  // a bright disc: a ring of aliased edge pixels at every angle
                r = 0.9f;
                g = 0.8f;
                b = 0.2f;
            }
            if (static_cast<float>(y) + 0.5f > 0.4f * (static_cast<float>(x) + 0.5f) + 20.0f && x > 38) {  // a dark slanted half-plane
                r = 0.05f;
                g = 0.05f;
                b = 0.1f;
            }
            if (y == 10 && x > 30) r = g = b = 1.0f;  // a thin horizontal line
            if (x == 55 && y < 18) r = g = b = 0.0f;  // a thin vertical line
            put(x, y, r, g, b);
        }
    }
    return px;
}

int encode_srgb_byte(float linear)
{
    const float c = std::clamp(linear, 0.0f, 1.0f);
    const float e = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return static_cast<int>(std::lround(e * 255.0f));
}

struct Parity {
    int compared = 0, bad = 0, changed = 0;
    int worst = 0;
};

Parity run_parity(bool srgb, const FxaaSettings& settings)
{
    const auto device = make_validated_device(srgb ? "fxaa_srgb" : "fxaa_unorm");
    const rhi::Format format = srgb ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8_UNORM;
    const std::vector<uint8_t> bytes = make_pixels();
    rhi::TextureDesc src;
    src.format = format;
    src.width = kW;
    src.height = kH;
    src.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    src.debug_name = "FxaaSource";
    const rhi::TextureHandle source = device->create_texture(src);
    device->upload_texture(source, std::as_bytes(std::span(bytes)));
    rhi::TextureDesc dst;
    dst.format = format;
    dst.width = kW;
    dst.height = kH;
    dst.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
    dst.debug_name = "FxaaTarget";
    const rhi::TextureHandle target = device->create_texture(dst);
    rhi::BufferDesc bd;
    bd.size = uint64_t{kW} * kH * 4;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryLocation::GpuToCpu;
    const rhi::BufferHandle readback = device->create_buffer(bd);
    Parity out;
    {
        FxaaPass pass(*device, format);
        const rhi::BindGroupHandle input = pass.create_input(source);
        rhi::CommandList& cmd = device->begin_frame();
        const rhi::ResourceState attach{rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::ColorAttachment};
        const rhi::TextureBarrier to_attachment{target, {rhi::PipelineStage::AllCommands, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::Undefined}, attach, {}};
        cmd.barrier(std::span(&to_attachment, 1));
        rhi::RenderingDesc rd;
        rhi::ColorAttachment ca;
        ca.texture = target;
        ca.load = rhi::AttachmentLoadOp::DontCare;
        rd.colors.push_back(ca);
        rd.area = {0, 0, kW, kH};
        cmd.begin_rendering(rd);
        cmd.set_viewport({0.0f, 0.0f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f});
        cmd.set_scissor({0, 0, kW, kH});
        pass.draw(cmd, input, settings, kW, kH);
        cmd.end_rendering();
        const rhi::TextureBarrier to_transfer{target, attach, {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
        cmd.barrier(std::span(&to_transfer, 1));
        cmd.copy_texture_to_buffer(target, readback);
        (void)device->end_frame();
        device->wait_idle();
        const auto mapped = device->map(readback);

        // The reference starts from the decoded bytes: display values, or their linear equivalents for an sRGB texture.
        FxaaImage ref(kW, kH);
        for (uint32_t i = 0; i < kW * kH; ++i) {
            const uint8_t* p = &bytes[std::size_t{i} * 4];
            ref.pixels[i] = srgb ? Float3{srgb_to_linear(p[0]), srgb_to_linear(p[1]), srgb_to_linear(p[2])} : Float3{p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f};
        }
        const FxaaImage want = fxaa_apply(ref, settings, pass.luma_exponent());
        for (uint32_t i = 0; i < kW * kH; ++i) {
            const float w3[3] = {want.pixels[i].x, want.pixels[i].y, want.pixels[i].z};
            bool differs = false;
            for (uint32_t k = 0; k < 3; ++k) {
                const int expected = srgb ? encode_srgb_byte(w3[k]) : static_cast<int>(std::lround(std::clamp(w3[k], 0.0f, 1.0f) * 255.0f));
                const int actual = std::to_integer<int>(mapped[i * 4 + k]);
                const int original = bytes[std::size_t{i} * 4 + k];
                ++out.compared;
                out.worst = std::max(out.worst, std::abs(actual - expected));
                if (std::abs(actual - expected) > 2) {
                    if (++out.bad <= 5) std::printf("  %s pixel %u channel %u: gpu %d, reference %d (source %d)\n", srgb ? "sRGB" : "UNORM", i, k, actual, expected, original);
                }
                differs = differs || actual != original;
            }
            out.changed += differs ? 1 : 0;
        }
        device->destroy(input);
    }
    device->destroy(readback);
    device->destroy(target);
    device->destroy(source);
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
    return out;
}

}  // namespace

TEST_CASE("FXAA pass on the GPU matches the CPU reference texel by texel, on a UNORM and on an sRGB target")
{
    FxaaSettings tuned;  // every parameter is a real input: the push constants reach the shader
    tuned.edge_threshold = 0.05f;
    tuned.edge_threshold_min = 0.02f;
    tuned.span_max = 2.5f;
    for (const FxaaSettings& settings : {FxaaSettings{}, tuned}) {
      for (const bool srgb : {false, true}) {
        const Parity p = run_parity(srgb, settings);
        std::printf("  %s%s: %d channels compared, worst difference %d levels, %d pixels changed by the pass\n", srgb ? "sRGB" : "UNORM", settings.span_max == 8.0f ? "" : " (tuned)", p.compared, p.worst, p.changed);
        CHECK(p.compared == static_cast<int>(kW * kH * 3));
        // Hardware bilinear weights are not the reference's float weights to the last bit, so a pixel whose blend sits exactly on the decision "wider blend or narrower" can
        // legitimately flip: a handful of channels may differ by more than two levels, none by much.
        CHECK(p.bad <= 6);
        CHECK(p.worst <= 12);
        CHECK(p.changed >= 60);              // the disc's ring, the slanted edge and the others really are smoothed (not a vacuous comparison)
        CHECK(p.changed <= static_cast<int>(kW * kH / 3));  // and most of the image is flat and untouched
      }
    }
}

namespace {

constexpr uint32_t kSceneW = 160;
constexpr uint32_t kSceneH = 120;

Image render_scene(bool hdr, bool fxaa, uint32_t tick = 0)
{
    const auto device = make_validated_device("fxaa_scene", /*synchronization=*/true);  // the intermediate target's barriers are checked
    RendererSettings s = deterministic_settings();
    s.hdr = hdr;
    s.tone_map.op = ToneMapOperator::Linear;
    s.fxaa.enabled = fxaa;
    SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, s);
    OffscreenTarget target(*device, kSceneW, kSceneH);
    std::vector<SceneInstance> cubes(3);
    cubes[0].position = {0.0f, 0.0f, 0.0f};
    cubes[1].position = {2.0f, 1.5f, 0.0f};
    cubes[1].color = {0.2f, 0.7f, 0.3f};
    cubes[1].yaw_offset = 0.6f;
    cubes[2].position = {-1.5f, -2.0f, 0.0f};
    cubes[2].color = {0.2f, 0.3f, 0.9f};
    cubes[2].yaw_offset = 1.1f;
    const Camera camera = look_at_camera({-6.0f, -5.0f, 4.0f}, {0.0f, 0.0f, 0.5f});
    const Image img = target.render(renderer, cubes, camera, tick, {});
    CHECK(device->validation_error_count() == 0);
    return img;
}

}  // namespace

TEST_CASE("FXAA through the scene renderer: flat neighbourhoods are unchanged, edges are smoothed, results are deterministic, in LDR and in HDR")
{
    for (const bool hdr : {false, true}) {
        const Image plain = render_scene(hdr, false);
        const Image smooth = render_scene(hdr, true);
        const Image again = render_scene(hdr, true);
        CHECK(smooth.rgba == again.rgba);
        uint32_t flat = 0, flat_changed = 0, changed = 0, edge_pixels = 0;
        for (uint32_t y = 0; y < kSceneH; ++y) {
            for (uint32_t x = 0; x < kSceneW; ++x) {
                const int d = max_channel_delta(plain.at(x, y), smooth.at(x, y));
                changed += d > 0 ? 1u : 0u;
                // The 5 x 5 neighbourhood of the *unsmoothed* image: a neighbourhood with almost no contrast has no edge in it, and FXAA's taps (the four diagonals and blends of
                // at most a few texels along an edge) all land inside it.
                int contrast = 0;
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int px = std::clamp(static_cast<int>(x) + dx, 0, static_cast<int>(kSceneW) - 1), py = std::clamp(static_cast<int>(y) + dy, 0, static_cast<int>(kSceneH) - 1);
                        contrast = std::max(contrast, max_channel_delta(plain.at(x, y), plain.at(static_cast<uint32_t>(px), static_cast<uint32_t>(py))));
                    }
                }
                if (contrast <= 6) {
                    ++flat;
                    flat_changed += d > 0 ? 1u : 0u;
                } else if (contrast > 40) {
                    ++edge_pixels;
                }
            }
        }
        std::printf("  %s: %u flat pixels (%u changed), %u edge pixels, %u pixels changed in all\n", hdr ? "HDR" : "LDR", flat, flat_changed, edge_pixels, changed);
        CHECK(flat >= 2500);  // (the ground is a checkerboard: much of the image is edges)
        CHECK(flat_changed == 0);   // nothing is touched away from an edge
        CHECK(edge_pixels >= 150);  // the cubes' silhouettes really are edges here
        CHECK(changed >= 60);       // and they are smoothed
        CHECK(changed <= edge_pixels * 3);  // within a narrow band around them
    }
}
