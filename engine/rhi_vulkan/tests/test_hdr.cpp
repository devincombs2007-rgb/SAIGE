// HDR output (milestone M4) on a validated Vulkan device: the scene drawn in linear light into an RGBA16_FLOAT target and
// resolved to the output by the tone-map pass. CI: lavapipe. Validation (with synchronization checks) errors fail each test.
//
// What is checked:
//   * with the Linear operator and no exposure, the HDR path reproduces the 0.2 LDR path (cubes, ground and imported models)
//     to within the 8-bit quantization both end in, so enabling HDR does not by itself change a scene;
//   * values above 1 survive until the tone map and then follow the CPU reference `tone_map` (the LDR path clips them);
//   * exposure scales the scene color by 2^EV before the operator;
//   * resizing the output and rendering many frames keeps the HDR target's state tracking valid.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/tonemap.hpp"
#include "saige/render/transform.hpp"
#include "model_test_util.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 192;
constexpr uint32_t kH = 108;

RendererSettings settings(bool hdr, ToneMapSettings tone_map = {})
{
    RendererSettings s = deterministic_settings();
    s.fog_density = 0.0f;
    s.hdr = hdr;
    s.tone_map = tone_map;
    return s;
}

struct Rig {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    std::unique_ptr<OffscreenTarget> target;

    Rig(const char* name, const RendererSettings& s, uint32_t width = kW, uint32_t height = kH)
    {
        device = make_validated_device(name, /*synchronization=*/true);
        renderer = std::make_unique<SceneRenderer>(*device, rhi::Format::RGBA8_SRGB, s);
        target = std::make_unique<OffscreenTarget>(*device, width, height);
    }
    ~Rig()
    {
        target.reset();
        renderer.reset();
    }
    GpuMaterial emissive_material(float factor)
    {
        MaterialData data;
        data.base_color[0] = data.base_color[1] = data.base_color[2] = 0.0f;  // only emission: no lighting term
        data.emissive[0] = data.emissive[1] = data.emissive[2] = factor;
        return renderer->model_renderer().create_material(data, MaterialTextures{}, "hdr.emissive");
    }
};

float to_linear(uint8_t v)
{
    const float c = static_cast<float>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

int max_delta(const Image& a, const Image& b)
{
    int worst = 0;
    for (size_t i = 0; i < a.rgba.size(); ++i) {
        if (i % 4 == 3) {
            continue;  // alpha
        }
        worst = std::max(worst, std::abs(int{a.rgba[i]} - int{b.rgba[i]}));
    }
    return worst;
}

} // namespace

TEST_CASE("HDR: with the Linear operator and no exposure, the HDR path reproduces the LDR path")
{
    ToneMapSettings linear;
    linear.op = ToneMapOperator::Linear;
    Rig ldr("hdr_vs_ldr_a", settings(false));
    Rig hdr("hdr_vs_ldr_b", settings(true, linear));
    {
        std::vector<SceneInstance> cubes;
        cubes.push_back({{0.0f, 0.0f, 0.0f}, {0.85f, 0.30f, 0.10f}, 0.3f, 1.0f});
        cubes.push_back({{2.5f, -1.0f, 0.0f}, {0.10f, 0.70f, 0.30f}, 1.1f, 1.0f});
        const Camera camera = look_at_camera({-6.0f, -4.0f, 3.0f}, {0.5f, 0.0f, 0.5f});

        MaterialData grey;
        grey.base_color[0] = 0.6f; grey.base_color[1] = 0.55f; grey.base_color[2] = 0.5f;
        grey.emissive[0] = 0.05f;
        const GpuModel ldr_cube = make_cube(ldr.renderer->registry(), ldr.renderer->model_renderer().create_material(grey, MaterialTextures{}, "g"));
        const GpuModel hdr_cube = make_cube(hdr.renderer->registry(), hdr.renderer->model_renderer().create_material(grey, MaterialTextures{}, "g"));
        const ModelDraw ldr_draw{&ldr_cube, translation({-2.0f, 2.0f, 0.5f})};
        const ModelDraw hdr_draw{&hdr_cube, translation({-2.0f, 2.0f, 0.5f})};

        const Image a = ldr.target->render(*ldr.renderer, cubes, camera, 0, std::span(&ldr_draw, 1));
        const Image b = hdr.target->render(*hdr.renderer, cubes, camera, 0, std::span(&hdr_draw, 1));
        const int delta = max_delta(a, b);
        std::printf("  largest channel difference between the LDR and HDR paths: %d / 255\n", delta);
        CHECK(delta <= 2);  // the float intermediate and the second encode: at most a couple of 8-bit levels
        // Not vacuous: the scene has cubes, ground and a model, i.e. several distinct colors.
        CHECK(silhouette_pixels(a, Image{kW, kH, std::vector<uint8_t>(a.rgba.size(), 0)}, 24) > 1000);
    }
    CHECK(ldr.device->validation_error_count() == 0);
    CHECK(hdr.device->validation_error_count() == 0);
}

TEST_CASE("HDR: values above 1 survive to the tone map and follow the reference; the LDR path clips them")
{
    const float factors[] = {0.25f, 1.0f, 3.0f, 12.0f};
    const Camera camera = look_at_camera({-6.0f, 0.0f, 1.5f}, {0.0f, 0.0f, 1.5f});
    const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
    const auto pixel_of = [&](float y) {
        const Projected p = project_point(vp, {0.0f, y, 1.5f});
        const PixelPos px = ndc_to_pixel(p.ndc_x, p.ndc_y, static_cast<float>(kW), static_cast<float>(kH));
        return std::pair<int, int>{static_cast<int>(px.x), static_cast<int>(px.y)};
    };

    for (const ToneMapOperator op : {ToneMapOperator::AcesFitted, ToneMapOperator::Reinhard}) {
        for (const float ev : {0.0f, -1.0f}) {
            ToneMapSettings tm;
            tm.op = op;
            tm.exposure_ev = ev;
            tm.white_point = 8.0f;
            Rig hdr("hdr_range", settings(true, tm));
            Rig ldr("hdr_range_ldr", settings(false));
            {
                std::vector<GpuModel> hdr_quads, ldr_quads;
                std::vector<ModelDraw> hdr_draws, ldr_draws;
                for (size_t i = 0; i < 4; ++i) {
                    hdr_quads.push_back(make_quad(hdr.renderer->registry(), hdr.emissive_material(factors[i]), 0.0f, 1.5f, 0.7f));
                    ldr_quads.push_back(make_quad(ldr.renderer->registry(), ldr.emissive_material(factors[i]), 0.0f, 1.5f, 0.7f));
                }
                for (size_t i = 0; i < 4; ++i) {
                    const Mat4 place = translation({0.0f, 2.7f - 1.8f * static_cast<float>(i), 0.0f});
                    hdr_draws.push_back({&hdr_quads[i], place});
                    ldr_draws.push_back({&ldr_quads[i], place});
                }
                const Image h = hdr.target->render(*hdr.renderer, {}, camera, 0, hdr_draws);
                const Image l = ldr.target->render(*ldr.renderer, {}, camera, 0, ldr_draws);
                for (size_t i = 0; i < 4; ++i) {
                    const auto [px, py] = pixel_of(2.7f - 1.8f * static_cast<float>(i));
                    const uint8_t* got = h.at(static_cast<uint32_t>(px), static_cast<uint32_t>(py));
                    const Float3 want = tone_map({factors[i], factors[i], factors[i]}, tm);
                    CHECK(std::fabs(to_linear(got[0]) - want.x) < 0.01f);
                    CHECK(std::fabs(to_linear(got[1]) - want.y) < 0.01f);
                    CHECK(std::fabs(to_linear(got[2]) - want.z) < 0.01f);
                    // The LDR path clips to white from a factor of 1 up, so 3 and 12 are indistinguishable there.
                    const uint8_t* clipped = l.at(static_cast<uint32_t>(px), static_cast<uint32_t>(py));
                    CHECK(clipped[0] == (factors[i] >= 1.0f ? 255 : clipped[0]));
                }
                // The HDR path keeps them apart (not vacuous): the four outputs are strictly increasing in the unsaturated range.
                if (op == ToneMapOperator::Reinhard && ev < 0.0f) {
                    float previous = -1.0f;
                    for (size_t i = 0; i < 4; ++i) {
                        const auto [px, py] = pixel_of(2.7f - 1.8f * static_cast<float>(i));
                        const float v = to_linear(h.at(static_cast<uint32_t>(px), static_cast<uint32_t>(py))[0]);
                        CHECK(v > previous);
                        previous = v;
                    }
                }
            }
            CHECK(hdr.device->validation_error_count() == 0);
            CHECK(ldr.device->validation_error_count() == 0);
        }
    }
}

TEST_CASE("HDR: resizing the output and rendering many frames keeps the target's state valid")
{
    Rig rig("hdr_resize", settings(true), kW, kH);
    {
        const Camera camera = look_at_camera({-6.0f, -4.0f, 3.0f}, {0.5f, 0.0f, 0.5f});
        std::vector<SceneInstance> cubes{{{0.0f, 0.0f, 0.0f}, {0.85f, 0.30f, 0.10f}, 0.3f, 1.0f}};
        Image first;
        for (int frame = 0; frame < 6; ++frame) {
            const Image img = rig.target->render(*rig.renderer, cubes, camera, 0);
            if (frame == 0) {
                first = img;
            }
            CHECK(img.rgba == first.rgba);  // deterministic frame after frame, with the HDR target reused
        }
        // A different size: the HDR target and its tone-map input are recreated; then back to the first size.
        rig.target.reset();
        rig.target = std::make_unique<OffscreenTarget>(*rig.device, 96, 54);
        const Image small = rig.target->render(*rig.renderer, cubes, camera, 0);
        CHECK(small.width == 96);
        {
            // The resized HDR target must give exactly what a renderer that never saw another size gives at 96x54.
            Rig fresh("hdr_resize_reference", settings(true), 96, 54);
            const Image reference = fresh.target->render(*fresh.renderer, cubes, camera, 0);
            CHECK(small.rgba == reference.rgba);
            CHECK(silhouette_pixels(small, Image{96, 54, std::vector<uint8_t>(small.rgba.size(), 0)}, 24) > 500);
        }
        rig.target.reset();
        rig.target = std::make_unique<OffscreenTarget>(*rig.device, kW, kH);
        const Image again = rig.target->render(*rig.renderer, cubes, camera, 0);
        CHECK(again.rgba == first.rgba);
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("HDR: frames submitted back to back without waiting are ordered by the HDR target's tracked state")
{
    Rig rig("hdr_in_flight", settings(true));
    {
        const Camera camera = look_at_camera({-6.0f, -4.0f, 3.0f}, {0.5f, 0.0f, 0.5f});
        std::vector<SceneInstance> cubes{{{0.0f, 0.0f, 0.0f}, {0.85f, 0.30f, 0.10f}, 0.3f, 1.0f}};
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_SRGB;
        td.width = kW;
        td.height = kH;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        td.debug_name = "InFlightTarget";
        const rhi::TextureHandle color = rig.device->create_texture(td);
        // Every frame draws into the same HDR target and output; the next frame's clear must wait for the previous frame's
        // tone-map read (write after read), which only a barrier from the real final state provides. Nothing waits here.
        for (uint64_t frame = 0; frame < 8; ++frame) {
            SceneFrame sf;
            sf.tick = frame;
            sf.instances = cubes;
            sf.camera = &camera;
            RenderTarget target;
            target.texture = color;
            target.width = kW;
            target.height = kH;
            // The output is the host's: say what state it is in, as a swapchain image would be (the first frame's contents are
            // undefined), so the only hazard left to order is the renderer's own HDR target.
            const rhi::ResourceState written{rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite,
                                             rhi::TextureLayout::ColorAttachment};
            if (frame > 0) {
                target.initial_state = written;
            }
            target.final_state = written;
            rhi::CommandList& cmd = rig.device->begin_frame();
            (void)rig.renderer->render(cmd, sf, target);
            (void)rig.device->end_frame();
        }
        rig.device->wait_idle();
        if (!rig.device->info().synchronization_validation_enabled) {
            std::printf("  synchronization validation unavailable on this device: the ordering check is weaker here\n");
        }
        rig.device->destroy(color);
    }
    CHECK(rig.device->validation_error_count() == 0);
}
