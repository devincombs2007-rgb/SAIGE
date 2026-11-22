// The 0.3 renderer as a whole, end to end on a validated Vulkan device: glTF data (all five textures, PNG-encoded) through the
// importer's tangent generator and GPU staging, drawn with metallic-roughness shading, normal mapping, emissive and occlusion in
// HDR and tone mapped, then compared with the CPU reference (brdf_reference.hpp + tone_map). One pixel's expected value uses
// every piece, so a wrong color space, binding, frame or tone-map step changes it.
//
// Also: resizing the output, failed staging that must leave the renderer's resources untouched, and replacing a staged model
// while frames that use it are still in flight (shader resource lifetime). Exit code 77 when no Vulkan device is available.
// CI: Linux lavapipe and Windows lavapipe. Software Vulkan only; nothing here is physical-GPU evidence.

#include "saige/gfx_test/brdf_reference.hpp"
#include "saige/gfx_test/png.hpp"
#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/gltf.hpp"
#include "saige/gfx_assets/tangents.hpp"
#include "saige/gfx_scene/stage_model.hpp"
#include "saige/render/tonemap.hpp"
#include "saige/render/transform.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace saige;
using namespace saige::render;
namespace brdf = saige::test::brdf;

namespace {

constexpr uint32_t kW = 192;
constexpr uint32_t kH = 108;
constexpr float kPi = 3.14159265358979323846f;

RendererSettings combined_settings()
{
    RendererSettings s = test::deterministic_settings();
    s.fog_density = 0.0f;
    s.sky = {0.30f, 0.50f, 0.82f, 1.0f};
    s.model_shading = ModelShading::MetallicRoughness;
    s.hdr = true;
    s.tone_map.op = ToneMapOperator::AcesFitted;
    s.tone_map.exposure_ev = -0.5f;
    return s;
}

std::vector<uint8_t> png_1x1(uint8_t r, uint8_t g, uint8_t b)
{
    return test::make_png(1, 1, 8, 6, {r, g, b, 255}, 4);
}

float srgb_decode(uint8_t v)
{
    const float c = static_cast<float>(v) / 255.0f;
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float unorm_decode(uint8_t v) { return static_cast<float>(v) / 255.0f; }

// The texel values of the five source images. Image 0 is used twice, as base color (sRGB) and as occlusion (linear).
constexpr uint8_t kGray[3] = {128, 128, 128};
constexpr uint8_t kMr[3] = {255, 128, 64};        // R unused; G roughness; B metallic
constexpr uint8_t kNormal[3] = {198, 128, 234};   // x +0.553, y +0.004, z +0.835
constexpr uint8_t kEmissive[3] = {128, 64, 32};   // sRGB

/// A quad facing -X centered on (0, 0, 1.5), half extent 1, with texture coordinates like the model tests: u grows toward -Y.
gfx::GltfAsset make_asset(float base_factor = 1.0f)
{
    gfx::GltfAsset asset;
    const std::vector<uint8_t> images[4] = {png_1x1(kGray[0], kGray[1], kGray[2]), png_1x1(kMr[0], kMr[1], kMr[2]),
                                            png_1x1(kNormal[0], kNormal[1], kNormal[2]),
                                            png_1x1(kEmissive[0], kEmissive[1], kEmissive[2])};
    for (int i = 0; i < 4; ++i) {
        gfx::Image image;
        image.name = "image" + std::to_string(i);
        image.mime_type = "image/png";
        image.bytes = images[i];
        asset.images.push_back(std::move(image));
        asset.textures.push_back({i, 0});
    }
    gfx::Sampler nearest;
    nearest.mag_filter = 9728;
    nearest.min_filter = 9728;
    nearest.wrap_s = nearest.wrap_t = 33071;
    asset.samplers.push_back(nearest);

    gfx::Material m;
    m.name = "all five";
    m.base_color[0] = m.base_color[1] = m.base_color[2] = base_factor;
    m.metallic = 1.0f;
    m.roughness = 1.0f;
    m.emissive[0] = m.emissive[1] = m.emissive[2] = 0.5f;
    m.base_color_texture.texture = 0;
    m.metallic_roughness_texture.texture = 1;
    m.normal_texture.texture = 2;
    m.occlusion_texture.texture = 0;  // the same image as base color, read as linear data
    m.emissive_texture.texture = 3;
    asset.materials.push_back(m);

    gfx::Primitive p;
    p.positions = {0, +1, 2.5f, 0, -1, 2.5f, 0, +1, 0.5f, 0, -1, 0.5f};
    p.normals = {-1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0};
    p.uv0 = {0, 0, 1, 0, 0, 1, 1, 1};
    p.indices = {0, 2, 3, 0, 3, 1};
    p.material = 0;
    asset.meshes.push_back({"quad", {p}, {}});
    asset.instances.push_back({0, -1, Mat4::identity(), "quad"});
    // What the importer does for a primitive whose material has a normal texture.
    (void)gfx::generate_tangents(asset.meshes[0].primitives[0]);
    return asset;
}

struct Rig {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    explicit Rig(const char* name)
    {
        device = test::make_validated_device(name, /*synchronization=*/true);
        renderer = std::make_unique<SceneRenderer>(*device, rhi::Format::RGBA8_SRGB, combined_settings());
    }
    ~Rig() { renderer.reset(); }
};

/// The reference color of the quad's center pixel as seen from `camera_position`, after the tone map.
Float3 expected_center(const Float3& camera_position, float base_factor, const RendererSettings& s)
{
    const Float3 center{0.0f, 0.0f, 1.5f};
    const Float3 v = normalize(camera_position - center);
    // Frame from the importer's tangent for this quad: T = -Y (u grows toward -Y), B = +Z (up in the image), N = -X.
    const Float3 t{0.0f, -1.0f, 0.0f}, b{0.0f, 0.0f, 1.0f}, nrm{-1.0f, 0.0f, 0.0f};
    const float nx = unorm_decode(kNormal[0]) * 2.0f - 1.0f, ny = unorm_decode(kNormal[1]) * 2.0f - 1.0f,
                nz = unorm_decode(kNormal[2]) * 2.0f - 1.0f;
    const Float3 n = normalize(t * nx + b * ny + nrm * nz);

    const float base = srgb_decode(kGray[0]) * base_factor;  // image 0 as sRGB base color
    const brdf::Surface surface{{base, base, base}, unorm_decode(kMr[2]), unorm_decode(kMr[1])};
    const float occlusion = unorm_decode(kGray[0]);           // the same image as linear data
    const Float3 emissive{srgb_decode(kEmissive[0]) * 0.5f, srgb_decode(kEmissive[1]) * 0.5f, srgb_decode(kEmissive[2]) * 0.5f};

    const Float3 sun_dir = normalize(Float3{-0.40f, -0.55f, 0.73f});
    const Float3 sun = brdf::shade_directional(surface, n, v, sun_dir, Float3{1.00f, 0.95f, 0.85f} * kPi);
    const auto ambient_radiance = [&](Float3 d) {
        const float k = d.z * 0.5f + 0.5f;
        const Float3 ground{0.20f, 0.18f, 0.16f};
        const Float3 sky{s.sky.r * 0.6f, s.sky.g * 0.6f, s.sky.b * 0.6f};
        return ground * (1.0f - k) + sky * k;
    };
    const Float3 r = n * (2.0f * dot(n, v)) - v;
    const brdf::EnvBrdf ab = brdf::integrate_env_brdf(std::clamp(surface.roughness, brdf::min_roughness, 1.0f), std::max(dot(n, v), 1e-4f));
    const Float3 ambient = brdf::shade_ambient(surface, occlusion, ab, ambient_radiance(n), ambient_radiance(r));
    return tone_map(sun + ambient + emissive, s.tone_map);
}

float linear_channel(const uint8_t* px, int c) { return srgb_decode(px[c]); }

bool center_matches(const test::Image& img, const Camera& camera, const Float3& camera_position, float base_factor, const RendererSettings& s)
{
    const Mat4 vp = view_projection_matrix(camera, static_cast<float>(img.width) / static_cast<float>(img.height));
    const Projected p = project_point(vp, {0.0f, 0.0f, 1.5f});
    const PixelPos px = ndc_to_pixel(p.ndc_x, p.ndc_y, static_cast<float>(img.width), static_cast<float>(img.height));
    const uint8_t* got = img.at(static_cast<uint32_t>(px.x), static_cast<uint32_t>(px.y));
    const Float3 want = expected_center(camera_position, base_factor, s);
    const float tolerance = 0.012f + 0.015f * std::max({want.x, want.y, want.z});
    const bool ok = std::fabs(linear_channel(got, 0) - want.x) <= tolerance && std::fabs(linear_channel(got, 1) - want.y) <= tolerance &&
                    std::fabs(linear_channel(got, 2) - want.z) <= tolerance;
    if (!ok) {
        std::printf("  center pixel (%d,%d): got (%.3f %.3f %.3f) want (%.3f %.3f %.3f)\n", static_cast<int>(px.x), static_cast<int>(px.y),
                    static_cast<double>(linear_channel(got, 0)), static_cast<double>(linear_channel(got, 1)),
                    static_cast<double>(linear_channel(got, 2)), static_cast<double>(want.x), static_cast<double>(want.y),
                    static_cast<double>(want.z));
    }
    return ok;
}

constexpr Float3 kCameraPosition{-6.0f, 0.0f, 1.5f};

} // namespace

TEST_CASE("Combined: a staged glTF with all five textures matches the reference under PBR, normal mapping and HDR")
{
    Rig rig("combined_all_slots");
    {
        const gfx::GltfAsset asset = make_asset();
        // The importer's tangent for this quad is the frame the oracle assumes (T = -Y, handedness +1).
        const gfx::Primitive& prim = asset.meshes[0].primitives[0];
        REQUIRE(prim.tangents.size() == 16);
        CHECK(std::fabs(prim.tangents[0]) < 1e-4f);
        CHECK(std::fabs(prim.tangents[1] + 1.0f) < 1e-4f);
        CHECK(std::fabs(prim.tangents[3] - 1.0f) < 1e-4f);

        gfx::StageReport report;
        const GpuModel model = gfx::stage_model(*rig.renderer, asset, "combined", {}, &report);
        // Image 0 is both an sRGB base color and linear occlusion: two uploads. Plus MR, normal (linear) and emissive (sRGB).
        CHECK(report.textures == 5);
        CHECK(report.warnings.empty());

        const Camera camera = look_at_camera(kCameraPosition, {0.0f, 0.0f, 1.5f});
        test::OffscreenTarget target(*rig.device, kW, kH);
        const ModelDraw draw{&model, Mat4::identity()};
        const test::Image img = target.render(*rig.renderer, {}, camera, 0, std::span(&draw, 1));
        CHECK(center_matches(img, camera, kCameraPosition, 1.0f, rig.renderer->settings()));
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Combined: resizing the output keeps every slot, the HDR target and the tone map correct")
{
    Rig rig("combined_resize");
    {
        const gfx::GltfAsset asset = make_asset();
        const GpuModel model = gfx::stage_model(*rig.renderer, asset, "combined", {}, nullptr);
        const Camera camera = look_at_camera(kCameraPosition, {0.0f, 0.0f, 1.5f});
        const ModelDraw draw{&model, Mat4::identity()};
        const std::pair<uint32_t, uint32_t> sizes[] = {{kW, kH}, {96, 54}, {320, 180}, {kW, kH}};
        test::Image first;
        for (const auto& [w, h] : sizes) {
            test::OffscreenTarget target(*rig.device, w, h);
            const test::Image img = target.render(*rig.renderer, {}, camera, 0, std::span(&draw, 1));
            CHECK(center_matches(img, camera, kCameraPosition, 1.0f, rig.renderer->settings()));
            if (w == kW && first.rgba.empty()) {
                first = img;
            } else if (w == kW) {
                CHECK(img.rgba == first.rgba);  // back at the first size: identical to the first frame
            }
        }
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Combined: failed staging leaves the renderer's resources and the active model untouched")
{
    Rig rig("combined_failed_staging");
    {
        const gfx::GltfAsset asset = make_asset();
        const GpuModel model = gfx::stage_model(*rig.renderer, asset, "good", {}, nullptr);
        const Camera camera = look_at_camera(kCameraPosition, {0.0f, 0.0f, 1.5f});
        test::OffscreenTarget target(*rig.device, kW, kH);
        const ModelDraw draw{&model, Mat4::identity()};
        const test::Image before = target.render(*rig.renderer, {}, camera, 0, std::span(&draw, 1));
        const ResourceStats live_before = rig.renderer->registry().total_stats();

        for (int variant = 0; variant < 3; ++variant) {
            gfx::GltfAsset bad = make_asset();
            if (variant == 0) bad.images[0].bytes = {1, 2, 3, 4};        // a required base color image does not decode
            if (variant == 1) bad.materials[0].base_color_texture.texture = 99;  // a required base color texture is out of range
            if (variant == 2) bad.meshes[0].primitives[0].tangents.assign(7, 0.0f);  // malformed vertex data, after textures
            bool threw = false;
            try {
                (void)gfx::stage_model(*rig.renderer, bad, "bad", {}, nullptr);
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw);
            const ResourceStats now = rig.renderer->registry().total_stats();
            CHECK(now.live == live_before.live);  // everything the failed attempt created was released
        }
        const test::Image after = target.render(*rig.renderer, {}, camera, 0, std::span(&draw, 1));
        CHECK(after.rgba == before.rgba);
        CHECK(center_matches(after, camera, kCameraPosition, 1.0f, rig.renderer->settings()));
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Combined: replacing a staged model while frames that use it are in flight keeps every resource valid")
{
    Rig rig("combined_replacement");
    {
        GpuModel model = gfx::stage_model(*rig.renderer, make_asset(1.0f), "first", {}, nullptr);
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_SRGB;
        td.width = kW;
        td.height = kH;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        td.debug_name = "CombinedTarget";
        const rhi::TextureHandle color = rig.device->create_texture(td);
        const Camera camera = look_at_camera(kCameraPosition, {0.0f, 0.0f, 1.5f});
        const rhi::ResourceState written{rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite,
                                         rhi::TextureLayout::ColorAttachment};
        uint64_t frame_index = 0;
        const auto submit_without_waiting = [&] {
            const ModelDraw draw{&model, Mat4::identity()};
            SceneFrame frame;
            frame.tick = frame_index;
            frame.camera = &camera;
            frame.models = std::span(&draw, 1);
            RenderTarget target;
            target.texture = color;
            target.width = kW;
            target.height = kH;
            if (frame_index > 0) {
                target.initial_state = written;
            }
            target.final_state = written;
            rhi::CommandList& cmd = rig.device->begin_frame();
            (void)rig.renderer->render(cmd, frame, target);
            (void)rig.device->end_frame();
            ++frame_index;
        };
        for (int i = 0; i < 3; ++i) {
            submit_without_waiting();  // frames using the first model; nothing waits for them
        }
        // Replace the model: the first one's textures, samplers, bind groups and buffers are released while those frames may
        // still be running, and a frame using the replacement follows immediately.
        model = gfx::stage_model(*rig.renderer, make_asset(0.5f), "second", {}, nullptr);
        for (int i = 0; i < 3; ++i) {
            submit_without_waiting();
        }
        rig.device->wait_idle();
        rig.device->destroy(color);
        // The replacement renders as the reference says for its own base color factor.
        test::OffscreenTarget target(*rig.device, kW, kH);
        const ModelDraw draw{&model, Mat4::identity()};
        const test::Image img = target.render(*rig.renderer, {}, camera, 0, std::span(&draw, 1));
        CHECK(center_matches(img, camera, kCameraPosition, 0.5f, rig.renderer->settings()));
        CHECK_FALSE(center_matches(img, camera, kCameraPosition, 1.0f, rig.renderer->settings()));  // and it is not the first one
    }
    CHECK(rig.device->validation_error_count() == 0);
}

int main(int argc, char** argv)
{
    std::string reason;
    if (!rhi::vulkan_device_available(&reason)) {
        std::printf("SKIPPED: no usable Vulkan 1.3 device: %s\n", reason.c_str());
        return 77;
    }
    return saige::test::run_all(argc, argv);
}
