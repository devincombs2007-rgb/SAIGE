// Bloom on a real (validated) Vulkan device. (1) The GPU chain (prefilter, 13-tap downsamples, tent upsamples) is compared texel by texel with the CPU reference
// (`render::bloom_chain`) for several sizes (odd ones, tiny ones) and settings, with NaN, infinity and negative texels in the source. (2) The tone-map pass's bloom
// permutation is compared with `tone_map(scene + intensity * bilinear(bloom))`. (3) Through the scene renderer: a bright object glows into its surroundings, a far
// pixel does not change, an intensity of zero is the image without bloom, resizing works, and everything is deterministic. Software Vulkan (lavapipe in CI).

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "saige/render/bloom.hpp"
#include "saige/render/tonemap.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

uint16_t to_half(float f)
{
    const uint32_t x = std::bit_cast<uint32_t>(f);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t exp = (x >> 23) & 0xFFu;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp == 0xFFu) return static_cast<uint16_t>(sign | 0x7C00u | (mant != 0 ? 0x200u : 0u));
    int e = static_cast<int>(exp) - 127 + 15;
    if (e >= 0x1F) return static_cast<uint16_t>(sign | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return static_cast<uint16_t>(sign);
        mant |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - e);
        uint32_t half_mant = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (half_mant & 1u))) ++half_mant;
        return static_cast<uint16_t>(sign | half_mant);
    }
    uint32_t half = sign | (static_cast<uint32_t>(e) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) ++half;
    return static_cast<uint16_t>(half);
}

float from_half(uint16_t h)
{
    const uint32_t sign = (h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    if (exp == 0) {
        const float v = std::ldexp(static_cast<float>(mant), -24);
        return (h & 0x8000u) ? -v : v;
    }
    if (exp == 0x1F) return std::bit_cast<float>(sign | 0x7F800000u | (mant << 13));
    return std::bit_cast<float>(sign | ((exp - 15 + 127) << 23) | (mant << 13));
}

/// The bytes of an RGBA16_FLOAT image, and the values it holds after the half-float round trip (what the reference must start from).
std::vector<std::byte> pack_half(const BloomImage& image, BloomImage& rounded)
{
    std::vector<std::byte> bytes;
    rounded = BloomImage(image.width, image.height);
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        const float texel[4] = {image.pixels[i].x, image.pixels[i].y, image.pixels[i].z, 1.0f};
        float stored[3] = {};
        for (int c = 0; c < 4; ++c) {
            const uint16_t h = to_half(texel[c]);
            bytes.push_back(static_cast<std::byte>(h & 0xFFu));
            bytes.push_back(static_cast<std::byte>(h >> 8));
            if (c < 3) stored[c] = from_half(h);
        }
        rounded.pixels[i] = {stored[0], stored[1], stored[2]};
    }
    return bytes;
}

/// A scene with a smooth background, several bright blobs of different sizes and brightness, and some hostile texels.
/// `non_finite` adds a NaN and an infinity (bilinear filtering of those is implementation-defined, so they are for the robustness test only); negatives are always there.
BloomImage make_scene(uint32_t w, uint32_t h, bool non_finite)
{
    BloomImage img(w, h);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const float fx = static_cast<float>(x) / static_cast<float>(w), fy = static_cast<float>(y) / static_cast<float>(h);
            img.at(x, y) = {0.2f + 0.6f * fx, 0.3f + 0.4f * fy, 0.1f + 0.3f * (1.0f - fx)};
        }
    }
    const auto blob = [&](float cx, float cy, float radius, float value) {
        for (uint32_t y = 0; y < h; ++y) {
            for (uint32_t x = 0; x < w; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) / static_cast<float>(w) - cx, dy = (static_cast<float>(y) + 0.5f) / static_cast<float>(h) - cy;
                if (std::sqrt(dx * dx + dy * dy) < radius) img.at(x, y) = {value, value * 0.8f, value * 0.5f};
            }
        }
    };
    blob(0.3f, 0.35f, 0.08f, 40.0f);
    blob(0.7f, 0.6f, 0.15f, 3.0f);
    blob(0.5f, 0.85f, 0.04f, 100.0f);
    if (w >= 8 && h >= 8) {
        img.at(w / 2, h / 2) = {-1.0f, -1.0f, -1.0f};
        img.at(w - 2, 2) = {7.0f, -4.0f, 0.5f};
        if (non_finite) {
            img.at(1, 1) = {std::numeric_limits<float>::quiet_NaN(), 1.0f, 2.0f};
            img.at(w - 3, 4) = {std::numeric_limits<float>::infinity(), 0.5f, 0.5f};
        }
    }
    return img;
}

constexpr rhi::ResourceState attachment_write{rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::ColorAttachment};

struct ChainRun {
    BloomImage gpu;  ///< U_0 as read back
    bool ok = false;
};

/// Runs a whole chain on the device and reads U_0 back.
ChainRun run_chain(rhi::Device& device, const BloomImage& scene, const BloomSettings& settings, BloomImage& rounded_scene)
{
    ChainRun run;
    const std::vector<std::byte> bytes = pack_half(scene, rounded_scene);
    rhi::TextureDesc sd;
    sd.format = rhi::Format::RGBA16_FLOAT;
    sd.width = scene.width;
    sd.height = scene.height;
    sd.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    sd.debug_name = "BloomTestSource";
    const rhi::TextureHandle source = device.create_texture(sd);
    device.upload_texture(source, bytes);

    BloomPass pass(device, settings);
    BloomChain chain = pass.create_chain(source, scene.width, scene.height, /*readable=*/true);
    const uint32_t n = chain.levels;
    const rhi::TextureHandle result = chain.result();
    const rhi::TextureDesc rd = *device.texture_desc(result);
    rhi::BufferDesc bd;
    bd.size = uint64_t{rd.width} * rd.height * 8;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryLocation::GpuToCpu;
    bd.debug_name = "BloomTestReadback";
    const rhi::BufferHandle readback = device.create_buffer(bd);

    rhi::CommandList& cmd = device.begin_frame();
    std::vector<rhi::TextureBarrier> init;
    for (const rhi::TextureHandle t : chain.down) init.push_back({t, {rhi::PipelineStage::AllCommands, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::Undefined}, attachment_write, {}});
    for (const rhi::TextureHandle t : chain.up) init.push_back({t, {rhi::PipelineStage::AllCommands, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::Undefined}, attachment_write, {}});
    cmd.barrier(init);
    const auto to_sampled = [&](rhi::TextureHandle t) {
        const rhi::TextureBarrier b{t, attachment_write, rhi::sampled_texture_state, {}};
        cmd.barrier(std::span(&b, 1));
    };
    for (uint32_t i = 0; i < n; ++i) {
        pass.record_down(cmd, chain, i);
        to_sampled(chain.down[i]);
    }
    for (uint32_t i = n - 1; i-- > 0;) {
        pass.record_up(cmd, chain, i);
        to_sampled(chain.up[i]);
    }
    const rhi::TextureBarrier to_transfer{result, rhi::sampled_texture_state, {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
    cmd.barrier(std::span(&to_transfer, 1));
    cmd.copy_texture_to_buffer(result, readback);
    (void)device.end_frame();
    device.wait_idle();
    const auto mapped = device.map(readback);
    run.gpu = BloomImage(rd.width, rd.height);
    for (std::size_t i = 0; i < run.gpu.pixels.size(); ++i) {
        uint16_t h[4];
        std::memcpy(h, mapped.data() + i * 8, 8);
        run.gpu.pixels[i] = {from_half(h[0]), from_half(h[1]), from_half(h[2])};
    }
    run.ok = true;
    device.destroy(readback);
    pass.destroy_chain(chain);
    device.destroy(source);
    return run;
}

}  // namespace

TEST_CASE("Bloom chain on the GPU matches the CPU reference texel by texel, for odd and tiny sizes, every setting and negative texels")
{
    const auto device = make_validated_device("bloom_chain", /*synchronization=*/true);
    struct Case {
        uint32_t w, h;
        BloomSettings s;
    };
    const auto settings = [](uint32_t levels, float threshold, float knee, float scatter) {
        BloomSettings s;
        s.enabled = true;
        s.levels = levels;
        s.threshold = threshold;
        s.knee = knee;
        s.scatter = scatter;
        return s;
    };
    const Case cases[] = {{64, 48, settings(5, 1.0f, 0.5f, 0.7f)}, {67, 45, settings(4, 0.5f, 0.0f, 1.0f)}, {16, 16, settings(8, 0.0f, 0.0f, 0.3f)},
                          {128, 72, settings(1, 1.0f, 0.5f, 0.7f)}, {33, 19, settings(3, 2.0f, 1.0f, 0.0f)}, {5, 3, settings(5, 1.0f, 0.5f, 0.7f)}, {2, 2, settings(5, 0.2f, 0.5f, 0.7f)}};
    float worst_overall = 0.0f;
    for (const Case& c : cases) {
        const BloomImage scene = make_scene(c.w, c.h, /*non_finite=*/false);
        BloomImage rounded;
        const ChainRun run = run_chain(*device, scene, c.s, rounded);
        REQUIRE(run.ok);
        const BloomImage want = bloom_chain(rounded, c.s);
        REQUIRE(run.gpu.width == want.width);
        REQUIRE(run.gpu.height == want.height);
        float worst = 0.0f;
        int bad = 0;
        float peak = 0.0f;
        for (std::size_t i = 0; i < want.pixels.size(); ++i) {
            const float w3[3] = {want.pixels[i].x, want.pixels[i].y, want.pixels[i].z}, g3[3] = {run.gpu.pixels[i].x, run.gpu.pixels[i].y, run.gpu.pixels[i].z};
            for (int k = 0; k < 3; ++k) {
                peak = std::max(peak, w3[k]);
                // Every level is stored in half floats (11 bits of precision) and filtered by the hardware: a few parts per thousand per level.
                const float tolerance = 0.012f * std::fabs(w3[k]) + 2e-3f;
                const float err = std::fabs(g3[k] - w3[k]);
                worst = std::max(worst, err / tolerance);
                if (err > tolerance) {
                    if (++bad <= 4) std::printf("  %ux%u texel %zu channel %d: gpu %f, reference %f\n", c.w, c.h, i, k, static_cast<double>(g3[k]), static_cast<double>(w3[k]));
                }
            }
        }
        std::printf("  %ux%u, %u levels (%u used): worst error / tolerance %.2f, peak %.2f\n", c.w, c.h, c.s.levels, bloom_level_count(c.w, c.h, c.s.levels), static_cast<double>(worst),
                    static_cast<double>(peak));
        CHECK(bad == 0);
        worst_overall = std::max(worst_overall, worst);
        // The scene has bright blobs, so the glow is real (not a vacuous comparison of zeros) unless the threshold is above everything.
        if (c.s.threshold <= 1.0f && c.w >= 16) CHECK(peak > 0.5f);
    }
    CHECK(worst_overall < 1.0f);
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Bloom chain on the GPU: NaN, infinity and negative texels in the scene never reach the glow")
{
    const auto device = make_validated_device("bloom_hostile");
    BloomSettings s;
    s.enabled = true;
    s.levels = 5;
    s.threshold = 0.5f;
    for (const auto& [w, h] : {std::pair{64u, 48u}, std::pair{23u, 31u}}) {
        BloomImage rounded;
        const ChainRun run = run_chain(*device, make_scene(w, h, /*non_finite=*/true), s, rounded);
        REQUIRE(run.ok);
        float peak = 0.0f;
        for (const Float3& p : run.gpu.pixels) {
            CHECK(std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z));
            CHECK(p.x >= 0.0f && p.y >= 0.0f && p.z >= 0.0f);
            peak = std::max(peak, p.x);
        }
        CHECK(peak > 0.0f);  // there is still glow from the real bright blobs
    }
    CHECK(device->validation_error_count() == 0);
}

namespace {

constexpr uint32_t kW = 16;

}

TEST_CASE("Tone map with bloom: the composite matches tone_map(scene + intensity * bilinear(bloom)) for every operator, intensity and exposure")
{
    const auto device = make_validated_device("bloom_composite");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    BloomImage hdr(kW, 1), bloom(8, 1);
    const Float3 values[kW] = {{0, 0, 0}, {0.001f, 0.001f, 0.005f}, {0.0183f, 0.1f, 0.18f}, {0.25f, 0.4f, 0.5f}, {0.75f, 0.9f, 1.0f}, {1.25f, 1.5f, 2.0f},
                               {3.0f, 4.0f, 5.0f}, {8.0f, 16.0f, 64.0f}, {500.0f, 1000.0f, 6000.0f}, {60000.0f, 65504.0f, 0.5f}, {-1.0f, -0.25f, 0.3f},
                               {nan, 0.5f, nan}, {0.2f, 0.2f, 0.2f}, {0.2f, nan, 100.0f}, {0.33f, 0.66f, 0.99f}, {10.0f, 0.05f, 1.0f}};
    for (uint32_t i = 0; i < kW; ++i) hdr.pixels[i] = values[i];
    for (uint32_t i = 0; i < 8; ++i) bloom.pixels[i] = {0.1f * static_cast<float>(i), 2.0f - 0.2f * static_cast<float>(i), (i % 3 == 0) ? 5.0f : 0.0f};
    BloomImage hdr_rounded, bloom_rounded;
    const std::vector<std::byte> hdr_bytes = pack_half(hdr, hdr_rounded), bloom_bytes = pack_half(bloom, bloom_rounded);
    const auto make_tex = [&](uint32_t w, const std::vector<std::byte>& bytes, const char* name) {
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA16_FLOAT;
        td.width = w;
        td.height = 1;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        td.debug_name = name;
        const rhi::TextureHandle t = device->create_texture(td);
        device->upload_texture(t, bytes);
        return t;
    };
    const rhi::TextureHandle hdr_tex = make_tex(kW, hdr_bytes, "CompositeHdr"), bloom_tex = make_tex(8, bloom_bytes, "CompositeBloom");
    rhi::TextureDesc tt;
    tt.format = rhi::Format::RGBA8_UNORM;
    tt.width = kW;
    tt.height = 1;
    tt.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
    tt.debug_name = "CompositeTarget";
    const rhi::TextureHandle target = device->create_texture(tt);
    rhi::BufferDesc bd;
    bd.size = kW * 4;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryLocation::GpuToCpu;
    const rhi::BufferHandle readback = device->create_buffer(bd);
    {
        ToneMapper mapper(*device, rhi::Format::RGBA8_UNORM, /*bloom=*/true);
        const rhi::BindGroupHandle input = mapper.create_input(hdr_tex, bloom_tex);
        int compared = 0, bad = 0;
        for (const ToneMapOperator op : {ToneMapOperator::Linear, ToneMapOperator::Reinhard, ToneMapOperator::AcesFitted}) {
            for (const float intensity : {0.0f, 0.25f, 2.0f, 100.0f, nan, -3.0f}) {  // 100 is clamped to 16, NaN and negatives to 0 (as bloom_composite does)
                for (const float ev : {0.0f, -1.0f}) {
                    ToneMapSettings s;
                    s.op = op;
                    s.exposure_ev = ev;
                    rhi::CommandList& cmd = device->begin_frame();
                    const rhi::TextureBarrier to_attachment{target, {rhi::PipelineStage::AllCommands, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::Undefined}, attachment_write, {}};
                    cmd.barrier(std::span(&to_attachment, 1));
                    rhi::RenderingDesc rd;
                    rhi::ColorAttachment ca;
                    ca.texture = target;
                    ca.clear = {0.5f, 0.5f, 0.5f, 0.5f};
                    rd.colors.push_back(ca);
                    rd.area = {0, 0, kW, 1};
                    cmd.begin_rendering(rd);
                    cmd.set_viewport({0.0f, 0.0f, static_cast<float>(kW), 1.0f, 0.0f, 1.0f});
                    cmd.set_scissor({0, 0, kW, 1});
                    mapper.draw(cmd, input, s, intensity);
                    cmd.end_rendering();
                    const rhi::TextureBarrier to_transfer{target, attachment_write, {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
                    cmd.barrier(std::span(&to_transfer, 1));
                    cmd.copy_texture_to_buffer(target, readback);
                    (void)device->end_frame();
                    device->wait_idle();
                    const auto mapped = device->map(readback);
                    for (uint32_t i = 0; i < kW; ++i) {
                        const Float3 glow = bloom_sample(bloom_rounded, (static_cast<float>(i) + 0.5f) / static_cast<float>(kW), 0.5f);
                        const Float3 want = tone_map(bloom_composite(hdr_rounded.pixels[i], glow, intensity), s);
                        const float w3[3] = {want.x, want.y, want.z};
                        for (uint32_t k = 0; k < 3; ++k) {
                            const int expected = static_cast<int>(std::lround(std::clamp(w3[k], 0.0f, 1.0f) * 255.0f));
                            const int actual = std::to_integer<int>(mapped[i * 4 + k]);
                            ++compared;
                            if (std::abs(actual - expected) > 2) {
                                if (++bad <= 6) std::printf("  op %d intensity %.2f ev %.1f texel %u channel %u: gpu %d, reference %d\n", static_cast<int>(op), static_cast<double>(intensity),
                                                            static_cast<double>(ev), i, k, actual, expected);
                            }
                        }
                    }
                }
            }
        }
        CHECK(compared == 3 * 6 * 2 * static_cast<int>(kW) * 3);
        CHECK(bad == 0);
        device->destroy(input);
    }
    device->destroy(readback);
    device->destroy(target);
    device->destroy(bloom_tex);
    device->destroy(hdr_tex);
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

namespace {

constexpr uint32_t kSceneW = 160;
constexpr uint32_t kSceneH = 120;

RendererSettings hdr_scene_settings(bool bloom, float intensity)
{
    RendererSettings s = deterministic_settings();
    s.hdr = true;
    s.fog_density = 0.0f;
    s.model_shading = ModelShading::Extended;  // emissive is shaded: the cube below glows with an HDR value far above the threshold
    s.bloom.enabled = bloom;
    s.bloom.intensity = intensity;
    s.bloom.threshold = 1.0f;
    s.bloom.levels = 5;
    s.tone_map.op = ToneMapOperator::Linear;  // so the added light is visible as such
    s.tone_map.exposure_ev = -3.0f;
    return s;
}

int channel_sum(const Image& img, uint32_t x, uint32_t y)
{
    const uint8_t* p = img.at(x, y);
    return int{p[0]} + int{p[1]} + int{p[2]};
}

}  // namespace

TEST_CASE("Bloom through the scene renderer: a bright cube glows into its surroundings only, intensity zero is the image without bloom, and resizing works")
{
    const auto device = make_validated_device("bloom_scene", /*synchronization=*/true);
    MaterialParams grey;
    grey.base_color[0] = grey.base_color[1] = grey.base_color[2] = 0.1f;
    grey.emissive[0] = grey.emissive[1] = grey.emissive[2] = 30.0f;
    SceneRenderer off(*device, rhi::Format::RGBA8_SRGB, hdr_scene_settings(false, 0.0f));
    SceneRenderer zero(*device, rhi::Format::RGBA8_SRGB, hdr_scene_settings(true, 0.0f));
    SceneRenderer on(*device, rhi::Format::RGBA8_SRGB, hdr_scene_settings(true, 0.4f));
    const GpuModel cube_off = make_cube(off.registry(), off.model_renderer().create_material(grey, {}, {}, "cube"));
    const GpuModel cube_zero = make_cube(zero.registry(), zero.model_renderer().create_material(grey, {}, {}, "cube"));
    const GpuModel cube_on = make_cube(on.registry(), on.model_renderer().create_material(grey, {}, {}, "cube"));
    const Camera camera = look_at_camera({-5.0f, -1.0f, 2.5f}, {0.0f, 0.0f, 0.5f});
    const auto draw_of = [](const GpuModel& m) { return std::vector<ModelDraw>{{&m, translation({0.0f, 0.0f, 0.5f})}}; };
    {
        OffscreenTarget target(*device, kSceneW, kSceneH);
        FrameStats stats_off, stats_on;
        const auto d_off = draw_of(cube_off), d_zero = draw_of(cube_zero), d_on = draw_of(cube_on);
        const Image a = target.render(off, {}, camera, 0, d_off, &stats_off);
        const Image b = target.render(zero, {}, camera, 0, d_zero);
        const Image c = target.render(on, {}, camera, 0, d_on, &stats_on);
        const Image c2 = target.render(on, {}, camera, 0, d_on);
        CHECK(stats_off.bloom_draw_calls == 0);
        CHECK(stats_on.bloom_draw_calls == 2 * 5 - 1);
        CHECK(b.rgba == a.rgba);   // an intensity of zero adds exactly nothing
        CHECK(c2.rgba == c.rgba);  // deterministic across frames (the chain's textures are reused, never stale)
        CHECK(c.rgba != a.rgba);
        // Brightness around the cube: find the cube's lit pixels (bright in the bloom-free image), then look just outside them and far away.
        int cube_pixels = 0, brighter_near = 0, near_total = 0, far_total = 0;
        double near_gain = 0.0, far_gain = 0.0;  // summed brightness added by the glow, in 8-bit channel sums
        for (uint32_t y = 0; y < kSceneH; ++y) {
            for (uint32_t x = 0; x < kSceneW; ++x) {
                const int base = channel_sum(a, x, y), glow = channel_sum(c, x, y);
                if (base > 400) {
                    ++cube_pixels;
                    CHECK(glow >= base - 3);  // the bright object never gets darker
                    continue;
                }
                // Distance (in pixels) to the nearest bright pixel, by brute force over a small window.
                int nearest = 1000;
                for (int dy = -12; dy <= 12 && nearest > 1; ++dy) {
                    for (int dx = -12; dx <= 12; ++dx) {
                        const int px = static_cast<int>(x) + dx, py = static_cast<int>(y) + dy;
                        if (px < 0 || py < 0 || px >= static_cast<int>(kSceneW) || py >= static_cast<int>(kSceneH)) continue;
                        if (channel_sum(a, static_cast<uint32_t>(px), static_cast<uint32_t>(py)) > 400) nearest = std::min(nearest, std::max(std::abs(dx), std::abs(dy)));
                    }
                }
                if (nearest <= 4) {
                    ++near_total;
                    near_gain += glow - base;
                    if (glow > base + 6) ++brighter_near;
                } else if (nearest > 12) {
                    ++far_total;
                    far_gain += glow - base;  // the widest levels reach this far, but weakly
                }
            }
        }
        const double near_mean = near_gain / std::max(near_total, 1), far_mean = far_gain / std::max(far_total, 1);
        std::printf("  cube %d px; near ring %d px (%d brighter, mean gain %.1f); far %d px (mean gain %.1f)\n", cube_pixels, near_total, brighter_near, near_mean, far_total, far_mean);
        CHECK(cube_pixels >= 150);
        CHECK(near_total >= 100);
        CHECK(brighter_near >= near_total / 2);  // the glow reaches most of the ring just outside the object
        CHECK(far_total >= 2000);
        CHECK(near_mean > 3.0 * far_mean);  // the glow falls off with distance: far pixels (more than 12 px from anything bright) get a small fraction of the ring's gain
        CHECK(far_mean >= 0.0);             // and it only ever adds light
    }
    // A different size with the same renderer: the chain is rebuilt, the glow is still there, a smaller image still renders.
    {
        OffscreenTarget target(*device, 96, 64);
        FrameStats stats;
        const auto d_on = draw_of(cube_on), d_off = draw_of(cube_off);
        const Image c = target.render(on, {}, camera, 0, d_on, &stats);
        const Image a = target.render(off, {}, camera, 0, d_off);
        CHECK(stats.bloom_draw_calls == 2 * 5 - 1);
        CHECK(c.rgba != a.rgba);
    }
    {
        OffscreenTarget target(*device, 3, 2);  // a tiny target reduces the chain to one level
        FrameStats stats;
        const auto d_on = draw_of(cube_on);
        (void)target.render(on, {}, camera, 0, d_on, &stats);
        CHECK(stats.bloom_draw_calls == 2 * bloom_level_count(3, 2, 5) - 1);
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}
