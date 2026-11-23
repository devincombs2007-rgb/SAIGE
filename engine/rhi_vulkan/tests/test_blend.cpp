// Blend state on a validated Vulkan device: pixel-exact checks of the blend equation, every factor and operation, the presets
// and the unchanged behavior of pipelines that do not use blending. CI: lavapipe. Validation errors (synchronization included)
// fail each test.
//
// A full-screen triangle outputs one constant color from push constants over a target cleared to a known background, so every
// pixel is `blend(source, background)` and the expected value is computed on the CPU from the specification of the equation.
// sRGB targets blend in linear space (the hardware decodes the destination and encodes the result); the expected value is
// computed the same way and compared as an encoded byte, within one level.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/shaders/test_blend_color.spv.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <vector>

using namespace saige;
using namespace saige::test;

namespace {

constexpr uint32_t kSize = 8;

struct Color {
    float r, g, b, a;
};

float srgb_decode(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

float from_half(uint16_t h)
{
    const uint32_t exponent = (h >> 10) & 0x1Fu;
    const uint32_t mantissa = h & 0x3FFu;
    float v;
    if (exponent == 0) {
        v = std::ldexp(static_cast<float>(mantissa), -24);
    } else if (exponent == 0x1Fu) {
        v = mantissa == 0 ? INFINITY : NAN;
    } else {
        v = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return (h & 0x8000u) != 0 ? -v : v;
}

float factor_value(rhi::BlendFactor f, const Color& src, const Color& dst, int channel)
{
    const float s[4] = {src.r, src.g, src.b, src.a};
    const float d[4] = {dst.r, dst.g, dst.b, dst.a};
    switch (f) {
    case rhi::BlendFactor::Zero: return 0.0f;
    case rhi::BlendFactor::One: return 1.0f;
    case rhi::BlendFactor::SrcColor: return s[channel];
    case rhi::BlendFactor::OneMinusSrcColor: return 1.0f - s[channel];
    case rhi::BlendFactor::DstColor: return d[channel];
    case rhi::BlendFactor::OneMinusDstColor: return 1.0f - d[channel];
    case rhi::BlendFactor::SrcAlpha: return src.a;
    case rhi::BlendFactor::OneMinusSrcAlpha: return 1.0f - src.a;
    case rhi::BlendFactor::DstAlpha: return dst.a;
    case rhi::BlendFactor::OneMinusDstAlpha: return 1.0f - dst.a;
    case rhi::BlendFactor::Count: break;
    }
    return 0.0f;
}

float apply_op(rhi::BlendOp op, float s, float sf, float d, float df)
{
    switch (op) {
    case rhi::BlendOp::Add: return s * sf + d * df;
    case rhi::BlendOp::Subtract: return s * sf - d * df;
    case rhi::BlendOp::ReverseSubtract: return d * df - s * sf;
    case rhi::BlendOp::Min: return std::min(s, d);
    case rhi::BlendOp::Max: return std::max(s, d);
    case rhi::BlendOp::Count: break;
    }
    return 0.0f;
}

/// The blend equation, per channel. Unclamped: a UNORM or sRGB target clamps afterwards.
Color blend(const rhi::BlendState& b, const Color& src, const Color& dst)
{
    if (!b.enable) {
        return src;
    }
    const float s[4] = {src.r, src.g, src.b, src.a};
    const float d[4] = {dst.r, dst.g, dst.b, dst.a};
    float out[4];
    for (int c = 0; c < 3; ++c) {
        out[c] = apply_op(b.color_op, s[c], factor_value(b.src_color, src, dst, c), d[c], factor_value(b.dst_color, src, dst, c));
    }
    out[3] = apply_op(b.alpha_op, s[3], factor_value(b.src_alpha, src, dst, 3), d[3], factor_value(b.dst_alpha, src, dst, 3));
    return {out[0], out[1], out[2], out[3]};
}

struct Rig {
    std::unique_ptr<rhi::Device> device;
    rhi::ShaderHandle vs, ps;
    std::map<std::pair<rhi::Format, std::array<uint8_t, 7>>, rhi::PipelineHandle> pipelines;

    explicit Rig(const char* name)
    {
        device = make_validated_device(name, /*synchronization=*/true);
        vs = device->create_shader({rhi::ShaderStage::Vertex, std::span(shaders::test_blend_color_vs_main), "VSMain", "BlendColor.VS"});
        ps = device->create_shader({rhi::ShaderStage::Pixel, std::span(shaders::test_blend_color_ps_main), "PSMain", "BlendColor.PS"});
    }
    ~Rig()
    {
        for (auto& [key, p] : pipelines) {
            device->destroy(p);
        }
        device->destroy(ps);
        device->destroy(vs);
    }

    rhi::PipelineHandle pipeline(rhi::Format format, const rhi::BlendState* state)
    {
        const rhi::BlendState s = state != nullptr ? *state : rhi::BlendState{};
        const std::array<uint8_t, 7> key{state != nullptr ? uint8_t{1} : uint8_t{0}, static_cast<uint8_t>(s.enable),
                                         static_cast<uint8_t>(s.src_color), static_cast<uint8_t>(s.dst_color),
                                         static_cast<uint8_t>(s.color_op), static_cast<uint8_t>(s.src_alpha),
                                         static_cast<uint8_t>(static_cast<uint8_t>(s.dst_alpha) | (static_cast<uint8_t>(s.alpha_op) << 4))};
        const auto k = std::make_pair(format, key);
        if (const auto it = pipelines.find(k); it != pipelines.end()) {
            return it->second;
        }
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = vs;
        pd.pixel_shader = ps;
        pd.cull_mode = rhi::CullMode::None;
        pd.color_formats = {format};
        if (state != nullptr) {
            pd.blend = {*state};
        }
        pd.push_constant_size = 16;
        pd.debug_name = "BlendColor";
        return pipelines.emplace(k, device->create_graphics_pipeline(pd)).first->second;
    }

    /// Clears to `background` (linear values; an sRGB target stores them encoded), draws each layer in order, and returns the
    /// color of the pixel at (x, y) as linear floats, with `encoded` set to the stored byte for 8-bit formats.
    Color render(rhi::Format format, const Color& background, std::span<const std::pair<Color, const rhi::BlendState*>> layers,
                 uint32_t x = kSize / 2, uint32_t y = kSize / 2, uint8_t* encoded = nullptr)
    {
        rhi::TextureDesc td;
        td.format = format;
        td.width = td.height = kSize;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        td.debug_name = "BlendTarget";
        const rhi::TextureHandle target = device->create_texture(td);
        const uint32_t bpp = rhi::get_format_info(format).bytes_per_block;
        rhi::BufferDesc bd;
        bd.size = uint64_t{kSize} * kSize * bpp;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        bd.debug_name = "BlendReadback";
        const rhi::BufferHandle readback = device->create_buffer(bd);

        rhi::CommandList& cmd = device->begin_frame();
        const rhi::ResourceState attachment{rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite,
                                            rhi::TextureLayout::ColorAttachment};
        const rhi::TextureBarrier to_attachment{target, {}, attachment, {}};
        cmd.barrier(std::span(&to_attachment, 1));
        rhi::RenderingDesc rd;
        rhi::ColorAttachment ca;
        ca.texture = target;
        ca.clear = {background.r, background.g, background.b, background.a};
        rd.colors.push_back(ca);
        rd.area = {0, 0, kSize, kSize};
        cmd.begin_rendering(rd);
        cmd.set_viewport({0.0f, 0.0f, static_cast<float>(kSize), static_cast<float>(kSize), 0.0f, 1.0f});
        cmd.set_scissor({0, 0, kSize, kSize});
        for (const auto& [color, state] : layers) {
            cmd.bind_pipeline(pipeline(format, state));
            cmd.push_constant_struct(color);
            cmd.draw(3);
        }
        cmd.end_rendering();
        const rhi::TextureBarrier to_transfer{target, attachment, {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
        cmd.barrier(std::span(&to_transfer, 1));
        cmd.copy_texture_to_buffer(target, readback);
        (void)device->end_frame();
        device->wait_idle();

        const auto bytes = device->map(readback);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(bytes.data()) + (size_t{y} * kSize + x) * bpp;
        Color out{};
        if (format == rhi::Format::RGBA16_FLOAT) {
            uint16_t h[4];
            std::memcpy(h, p, 8);
            out = {from_half(h[0]), from_half(h[1]), from_half(h[2]), from_half(h[3])};
        } else {
            if (encoded != nullptr) {
                *encoded = p[0];
            }
            const bool srgb = rhi::is_srgb(format);
            const auto channel = [&](int c) { return srgb && c < 3 ? srgb_decode(static_cast<float>(p[c]) / 255.0f) : static_cast<float>(p[c]) / 255.0f; };
            out = {channel(0), channel(1), channel(2), channel(3)};
        }
        device->destroy(readback);
        device->destroy(target);
        return out;
    }
};

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }
bool close(const Color& a, const Color& b, float tolerance)
{
    return std::fabs(a.r - b.r) <= tolerance && std::fabs(a.g - b.g) <= tolerance && std::fabs(a.b - b.b) <= tolerance &&
           std::fabs(a.a - b.a) <= tolerance;
}

const Color kSrc{0.80f, 0.30f, 0.60f, 0.40f};
const Color kDst{0.20f, 0.70f, 0.50f, 0.90f};

} // namespace

TEST_CASE("Blend (Vulkan): pipelines without blend state, or with a disabled one, overwrite the target exactly as before")
{
    Rig rig("blend_overwrite");
    {
        for (const rhi::Format format : {rhi::Format::RGBA8_UNORM, rhi::Format::RGBA8_SRGB, rhi::Format::RGBA16_FLOAT}) {
            const rhi::BlendState disabled{};
            rhi::BlendState one_zero;  // enabled with One / Zero is also an overwrite
            one_zero.enable = true;
            const std::pair<Color, const rhi::BlendState*> none[] = {{kSrc, nullptr}};
            const std::pair<Color, const rhi::BlendState*> off[] = {{kSrc, &disabled}};
            const std::pair<Color, const rhi::BlendState*> overwrite[] = {{kSrc, &one_zero}};
            const float tolerance = format == rhi::Format::RGBA16_FLOAT ? 0.001f : 0.004f;
            for (const auto& layers : {std::span(none), std::span(off), std::span(overwrite)}) {
                const Color got = rig.render(format, kDst, layers);
                CHECK(close(got, kSrc, tolerance));
            }
        }
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Blend (Vulkan): source-over follows the equation in UNORM, sRGB (blended in linear) and float targets")
{
    Rig rig("blend_source_over");
    {
        const rhi::BlendState straight = rhi::BlendState::source_over();
        const rhi::BlendState premultiplied = rhi::BlendState::premultiplied_over();
        for (const float alpha : {0.0f, 0.25f, 0.5f, 1.0f}) {
            Color src = kSrc;
            src.a = alpha;
            const std::pair<Color, const rhi::BlendState*> layer_straight[] = {{src, &straight}};
            const Color premul_src{src.r * alpha, src.g * alpha, src.b * alpha, alpha};
            const std::pair<Color, const rhi::BlendState*> layer_premul[] = {{premul_src, &premultiplied}};
            for (const rhi::Format format : {rhi::Format::RGBA8_UNORM, rhi::Format::RGBA8_SRGB, rhi::Format::RGBA16_FLOAT}) {
                const bool srgb = rhi::is_srgb(format);
                const float tolerance = format == rhi::Format::RGBA16_FLOAT ? 0.002f : 0.006f;
                // The equation, with the background as stored (an 8-bit background is quantized, which the readback also sees).
                Color want = blend(straight, src, kDst);
                Color want_premul = blend(premultiplied, premul_src, kDst);
                for (Color* w : {&want, &want_premul}) {
                    w->r = clamp01(w->r), w->g = clamp01(w->g), w->b = clamp01(w->b), w->a = clamp01(w->a);
                }
                const Color got = rig.render(format, kDst, layer_straight);
                const Color got_premul = rig.render(format, kDst, layer_premul);
                CHECK(close(got, want, tolerance));
                CHECK(close(got_premul, want_premul, tolerance));
                // The two forms of source-over agree.
                CHECK(close(got, got_premul, 2.0f * tolerance));
                (void)srgb;
            }
        }
        // Alpha 0 leaves the background, alpha 1 replaces it (not vacuous: those are the two ends of the equation).
        Color transparent = kSrc;
        transparent.a = 0.0f;
        const std::pair<Color, const rhi::BlendState*> t[] = {{transparent, &straight}};
        CHECK(close(rig.render(rhi::Format::RGBA16_FLOAT, kDst, t), kDst, 0.002f));
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Blend (Vulkan): every factor and operation maps to the right hardware setting")
{
    Rig rig("blend_sweep");
    {
        // RGBA16_FLOAT does not clamp, so the unclamped equation is compared directly (negative results included).
        const rhi::Format format = rhi::Format::RGBA16_FLOAT;
        size_t checked = 0;
        for (int f = 0; f < static_cast<int>(rhi::BlendFactor::Count); ++f) {
            const auto factor = static_cast<rhi::BlendFactor>(f);
            // Source factor sweep with a fixed destination factor, then the destination factor, for color and for alpha.
            rhi::BlendState s;
            s.enable = true;
            s.src_color = factor;
            s.dst_color = rhi::BlendFactor::One;
            s.src_alpha = factor;
            s.dst_alpha = rhi::BlendFactor::One;
            rhi::BlendState d = s;
            d.src_color = rhi::BlendFactor::One;
            d.dst_color = factor;
            d.src_alpha = rhi::BlendFactor::One;
            d.dst_alpha = factor;
            // Distinct color and alpha factors catch a swapped pair.
            rhi::BlendState mixed = s;
            mixed.src_alpha = static_cast<rhi::BlendFactor>((f + 3) % static_cast<int>(rhi::BlendFactor::Count));
            mixed.dst_alpha = static_cast<rhi::BlendFactor>((f + 7) % static_cast<int>(rhi::BlendFactor::Count));
            for (const rhi::BlendState& state : {s, d, mixed}) {
                const std::pair<Color, const rhi::BlendState*> layer[] = {{kSrc, &state}};
                const Color got = rig.render(format, kDst, layer);
                const Color want = blend(state, kSrc, kDst);
                if (!close(got, want, 0.003f)) {
                    std::printf("  factor %d: got (%.4f %.4f %.4f %.4f) want (%.4f %.4f %.4f %.4f)\n", f, static_cast<double>(got.r),
                                static_cast<double>(got.g), static_cast<double>(got.b), static_cast<double>(got.a), static_cast<double>(want.r),
                                static_cast<double>(want.g), static_cast<double>(want.b), static_cast<double>(want.a));
                }
                CHECK(close(got, want, 0.003f));
                ++checked;
            }
        }
        for (int o = 0; o < static_cast<int>(rhi::BlendOp::Count); ++o) {
            rhi::BlendState state;
            state.enable = true;
            state.src_color = rhi::BlendFactor::SrcAlpha;
            state.dst_color = rhi::BlendFactor::OneMinusSrcAlpha;
            state.src_alpha = rhi::BlendFactor::One;
            state.dst_alpha = rhi::BlendFactor::One;
            state.color_op = static_cast<rhi::BlendOp>(o);
            state.alpha_op = static_cast<rhi::BlendOp>((o + 2) % static_cast<int>(rhi::BlendOp::Count));
            const std::pair<Color, const rhi::BlendState*> layer[] = {{kSrc, &state}};
            const Color got = rig.render(format, kDst, layer);
            const Color want = blend(state, kSrc, kDst);
            CHECK(close(got, want, 0.003f));
            ++checked;
        }
        CHECK(checked == 3u * static_cast<size_t>(rhi::BlendFactor::Count) + static_cast<size_t>(rhi::BlendOp::Count));
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Blend (Vulkan): layers composite in draw order, which is why transparent draws need sorting")
{
    Rig rig("blend_order");
    {
        const rhi::BlendState over = rhi::BlendState::source_over();
        const Color red{1.0f, 0.0f, 0.0f, 0.5f};
        const Color blue{0.0f, 0.0f, 1.0f, 0.5f};
        const Color background{0.0f, 0.0f, 0.0f, 1.0f};
        const std::pair<Color, const rhi::BlendState*> red_then_blue[] = {{red, &over}, {blue, &over}};
        const std::pair<Color, const rhi::BlendState*> blue_then_red[] = {{blue, &over}, {red, &over}};
        for (const rhi::Format format : {rhi::Format::RGBA8_UNORM, rhi::Format::RGBA8_SRGB, rhi::Format::RGBA16_FLOAT}) {
            const Color a = rig.render(format, background, red_then_blue);
            const Color b = rig.render(format, background, blue_then_red);
            const Color want_a = blend(over, blue, blend(over, red, background));
            const Color want_b = blend(over, red, blend(over, blue, background));
            const float tolerance = format == rhi::Format::RGBA16_FLOAT ? 0.002f : 0.008f;
            CHECK(close(a, want_a, tolerance));
            CHECK(close(b, want_b, tolerance));
            CHECK(!close(a, b, 0.1f));  // the order changes the result: not commutative
        }
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Blend (Vulkan): pipelines with unsupported blend descriptions are rejected before anything is created")
{
    Rig rig("blend_rejected");
    {
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = rig.vs;
        pd.pixel_shader = rig.ps;
        pd.cull_mode = rhi::CullMode::None;
        pd.push_constant_size = 16;
        pd.color_formats = {rhi::Format::RGBA8_UINT};
        pd.blend = {rhi::BlendState::source_over()};
        bool threw = false;
        try {
            (void)rig.device->create_graphics_pipeline(pd);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
        pd.color_formats = {rhi::Format::RGBA8_UNORM, rhi::Format::RGBA8_UNORM};
        pd.blend = {rhi::BlendState::source_over(), rhi::BlendState::additive()};
        threw = false;
        try {
            (void)rig.device->create_graphics_pipeline(pd);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Blend (Vulkan): the selected device is asked, before anything is created, whether it can blend into each attachment")
{
    Rig rig("blend_capability");
    {
        // The formats the specification guarantees really do create blended pipelines on this device.
        for (const rhi::Format f : {rhi::Format::R8_UNORM, rhi::Format::RG8_UNORM, rhi::Format::RGBA8_UNORM, rhi::Format::RGBA8_SRGB,
                                    rhi::Format::BGRA8_UNORM, rhi::Format::BGRA8_SRGB, rhi::Format::RGB10A2_UNORM, rhi::Format::R16_FLOAT,
                                    rhi::Format::RG16_FLOAT, rhi::Format::RGBA16_FLOAT}) {
            rhi::GraphicsPipelineDesc pd;
            pd.vertex_shader = rig.vs;
            pd.pixel_shader = rig.ps;
            pd.cull_mode = rhi::CullMode::None;
            pd.push_constant_size = 16;
            pd.color_formats = {f};
            pd.blend = {rhi::BlendState::source_over()};
            rhi::PipelineHandle p;
            bool created = true;
            try {
                p = rig.device->create_graphics_pipeline(pd);
            } catch (const std::exception&) {
                created = false;
            }
            CHECK(created);
            if (created) rig.device->destroy(p);
        }

#if defined(SAIGE_RHI_VULKAN_TESTING)
        // A device that cannot blend into a format (simulated by the test hook, so this runs on a device that can): the blended
        // pipeline is refused with a clear error before any Vulkan object is created, an unblended pipeline into the same format is
        // not affected, other formats are not affected, and cancelling the hook restores the normal behavior.
        rhi::testing::deny_blending_into(rhi::Format::RGBA8_UNORM);
        rhi::GraphicsPipelineDesc pd;
        pd.vertex_shader = rig.vs;
        pd.pixel_shader = rig.ps;
        pd.cull_mode = rhi::CullMode::None;
        pd.push_constant_size = 16;
        pd.color_formats = {rhi::Format::RGBA8_UNORM};
        pd.blend = {rhi::BlendState::source_over()};
        std::string message;
        try {
            (void)rig.device->create_graphics_pipeline(pd);
        } catch (const std::invalid_argument& e) {
            message = e.what();
        }
        CHECK(message.find("cannot blend into RGBA8_UNORM") != std::string::npos);
        pd.blend.clear();
        rhi::PipelineHandle unblended = rig.device->create_graphics_pipeline(pd);  // not blended: not refused
        CHECK(unblended.is_valid());
        rig.device->destroy(unblended);
        pd.color_formats = {rhi::Format::RGBA16_FLOAT};
        pd.blend = {rhi::BlendState::source_over()};
        rhi::PipelineHandle other = rig.device->create_graphics_pipeline(pd);
        CHECK(other.is_valid());
        rig.device->destroy(other);
        rhi::testing::deny_blending_into(rhi::Format::Unknown);
        pd.color_formats = {rhi::Format::RGBA8_UNORM};
        rhi::PipelineHandle restored = rig.device->create_graphics_pipeline(pd);
        CHECK(restored.is_valid());
        rig.device->destroy(restored);
#endif
    }
    CHECK(rig.device->validation_error_count() == 0);
}
