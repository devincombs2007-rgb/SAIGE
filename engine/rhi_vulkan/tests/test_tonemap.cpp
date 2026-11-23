// The tone-mapping pass on a real (validated) device: a floating-point texture of known values goes through the pass into an
// 8-bit target and every pixel is compared with the CPU reference (`render::tone_map`) for each operator and several exposures.
// Non-finite and negative HDR values are included. CI: lavapipe. Validation errors fail each test.

#include "saige/gfx_test/saige_test.hpp"

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
using namespace saige::render;

namespace {

constexpr uint32_t kWidth = 16;

/// IEEE binary32 -> binary16 with round-to-nearest-even; handles zero, subnormals, infinity and NaN.
uint16_t to_half(float f)
{
    const uint32_t x = std::bit_cast<uint32_t>(f);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const uint32_t exp = (x >> 23) & 0xFFu;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp == 0xFFu) {
        return static_cast<uint16_t>(sign | 0x7C00u | (mant != 0 ? 0x200u : 0u));
    }
    int e = static_cast<int>(exp) - 127 + 15;
    if (e >= 0x1F) {
        return static_cast<uint16_t>(sign | 0x7C00u);  // overflow to infinity
    }
    if (e <= 0) {
        if (e < -10) {
            return static_cast<uint16_t>(sign);
        }
        mant |= 0x800000u;
        const uint32_t shift = static_cast<uint32_t>(14 - e);
        uint32_t half_mant = mant >> shift;
        const uint32_t rem = mant & ((1u << shift) - 1u);
        const uint32_t halfway = 1u << (shift - 1);
        if (rem > halfway || (rem == halfway && (half_mant & 1u))) {
            ++half_mant;
        }
        return static_cast<uint16_t>(sign | half_mant);
    }
    uint32_t half = sign | (static_cast<uint32_t>(e) << 10) | (mant >> 13);
    const uint32_t rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (half & 1u))) {
        ++half;
    }
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
    if (exp == 0x1F) {
        return std::bit_cast<float>(sign | 0x7F800000u | (mant << 13));
    }
    return std::bit_cast<float>(sign | ((exp - 15 + 127) << 23) | (mant << 13));
}

float srgb_encode(float linear)
{
    const float c = std::clamp(linear, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

/// 16 HDR texels (RGBA), chosen to cross every operator's knee. Stored as binary16, so the reference uses the rounded values.
std::vector<Float3> hdr_values()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    return {
        {0.0f, 0.0f, 0.0f},      {0.0009765625f, 0.001f, 0.0049f}, {0.0183f, 0.1f, 0.18f},   {0.25f, 0.4f, 0.5f},
        {0.75f, 0.9f, 1.0f},     {1.25f, 1.5f, 2.0f},              {3.0f, 4.0f, 5.0f},        {8.0f, 16.0f, 64.0f},
        {500.0f, 1000.0f, 6000.0f}, {60000.0f, 65504.0f, 0.5f},    {-1.0f, -0.25f, 0.3f},     {nan, 0.5f, nan},
        {inf, 0.5f, -inf},       {0.2f, nan, 100.0f},              {0.33f, 0.66f, 0.99f},     {10.0f, 0.05f, 1.0f},
    };
}

struct Rig {
    rhi::Device& device;
    rhi::Format target_format;
    rhi::TextureHandle hdr;
    rhi::TextureHandle target;
    rhi::BufferHandle readback;
    std::vector<Float3> reference_input;  // what the texture actually holds (after the half-float round trip)
    uint32_t bytes_per_texel;

    Rig(rhi::Device& d, rhi::Format format) : device(d), target_format(format), bytes_per_texel(format == rhi::Format::RGBA16_FLOAT ? 8u : 4u)
    {
        const std::vector<Float3> values = hdr_values();
        std::vector<std::byte> bytes;
        for (const Float3& v : values) {
            const float texel[4] = {v.x, v.y, v.z, 1.0f};
            Float3 stored{};
            float* out[3] = {&stored.x, &stored.y, &stored.z};
            for (int c = 0; c < 4; ++c) {
                const uint16_t h = to_half(texel[c]);
                bytes.push_back(static_cast<std::byte>(h & 0xFFu));
                bytes.push_back(static_cast<std::byte>(h >> 8));
                if (c < 3) *out[c] = from_half(h);
            }
            reference_input.push_back(stored);
        }
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA16_FLOAT;
        td.width = kWidth;
        td.height = 1;
        td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        td.debug_name = "TonemapHdr";
        hdr = device.create_texture(td);
        device.upload_texture(hdr, bytes);
        rhi::TextureDesc tt;
        tt.format = format;
        tt.width = kWidth;
        tt.height = 1;
        tt.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        tt.debug_name = "TonemapTarget";
        target = device.create_texture(tt);
        rhi::BufferDesc bd;
        bd.size = kWidth * bytes_per_texel;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        bd.debug_name = "TonemapReadback";
        readback = device.create_buffer(bd);
    }
    ~Rig()
    {
        device.destroy(readback);
        device.destroy(target);
        device.destroy(hdr);
    }

    std::vector<uint8_t> run(ToneMapper& mapper, const ToneMapSettings& settings)
    {
        const rhi::BindGroupHandle input = mapper.create_input(hdr);
        rhi::CommandList& cmd = device.begin_frame();
        const rhi::TextureBarrier to_attachment{
            target,
            {rhi::PipelineStage::AllCommands, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::Undefined},
            {rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::ColorAttachment},
            {}};
        cmd.barrier(std::span(&to_attachment, 1));
        rhi::RenderingDesc rd;
        rhi::ColorAttachment ca;
        ca.texture = target;
        ca.clear = {0.5f, 0.5f, 0.5f, 0.5f};  // a visible sentinel: an unwritten pixel would stay 128
        rd.colors.push_back(ca);
        rd.area = {0, 0, kWidth, 1};
        cmd.begin_rendering(rd);
        cmd.set_viewport({0.0f, 0.0f, static_cast<float>(kWidth), 1.0f, 0.0f, 1.0f});
        cmd.set_scissor({0, 0, kWidth, 1});
        mapper.draw(cmd, input, settings);
        cmd.end_rendering();
        const rhi::TextureBarrier to_transfer{target, to_attachment.after,
                                              {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc}, {}};
        cmd.barrier(std::span(&to_transfer, 1));
        cmd.copy_texture_to_buffer(target, readback);
        (void)device.end_frame();
        device.wait_idle();
        const auto mapped = device.map(readback);
        std::vector<uint8_t> pixels(kWidth * bytes_per_texel);
        std::memcpy(pixels.data(), mapped.data(), pixels.size());
        device.destroy(input);
        return pixels;
    }
};

int expected_byte(float linear, bool srgb)
{
    const float v = srgb ? srgb_encode(linear) : std::clamp(linear, 0.0f, 1.0f);
    return static_cast<int>(std::lround(v * 255.0f));
}

/// Number of channel mismatches beyond `tolerance` between the GPU output and the reference.
int mismatches(const Rig& rig, const std::vector<uint8_t>& pixels, const ToneMapSettings& s, bool srgb, int tolerance)
{
    int bad = 0;
    for (uint32_t i = 0; i < kWidth; ++i) {
        const Float3 want = tone_map(rig.reference_input[i], s);
        const float channels[3] = {want.x, want.y, want.z};
        for (int c = 0; c < 3; ++c) {
            const int expected = expected_byte(channels[c], srgb);
            const int actual = pixels[i * 4 + static_cast<uint32_t>(c)];
            if (std::abs(actual - expected) > tolerance) {
                std::printf("  texel %u channel %d: gpu %d, reference %d (input %f)\n", i, c, actual, expected,
                            static_cast<double>(c == 0 ? rig.reference_input[i].x : c == 1 ? rig.reference_input[i].y : rig.reference_input[i].z));
                ++bad;
            }
        }
        if (pixels[i * 4 + 3] != 255) {
            ++bad;  // alpha is opaque
        }
    }
    return bad;
}

} // namespace

TEST_CASE("Tone map pass: every operator and exposure matches the CPU reference on a UNORM target")
{
    const auto device = test::make_validated_device("tonemap_unorm");
    {
        Rig rig(*device, rhi::Format::RGBA8_UNORM);
        ToneMapper mapper(*device, rhi::Format::RGBA8_UNORM);
        for (const ToneMapOperator op : {ToneMapOperator::Linear, ToneMapOperator::Reinhard, ToneMapOperator::AcesFitted}) {
            for (const float ev : {-1.0f, 0.0f, 2.0f}) {
                ToneMapSettings s;
                s.op = op;
                s.exposure_ev = ev;
                s.white_point = 4.0f;
                const std::vector<uint8_t> pixels = rig.run(mapper, s);
                CHECK(mismatches(rig, pixels, s, false, 1) == 0);
            }
        }
        ToneMapSettings white;
        white.op = ToneMapOperator::Reinhard;
        white.white_point = 10.0f;
        CHECK(mismatches(rig, rig.run(mapper, white), white, false, 1) == 0);  // the white point is a real parameter
        // The three operators really differ on the same input (so the push constant reaches the shader).
        ToneMapSettings a{ToneMapOperator::Linear, 0.0f, 4.0f};
        ToneMapSettings b{ToneMapOperator::AcesFitted, 0.0f, 4.0f};
        CHECK(rig.run(mapper, a) != rig.run(mapper, b));
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Tone map pass: an sRGB target applies the display encoding after the operator")
{
    const auto device = test::make_validated_device("tonemap_srgb");
    {
        Rig rig(*device, rhi::Format::RGBA8_SRGB);
        ToneMapper mapper(*device, rhi::Format::RGBA8_SRGB);
        for (const ToneMapOperator op : {ToneMapOperator::Linear, ToneMapOperator::AcesFitted}) {
            ToneMapSettings s;
            s.op = op;
            s.exposure_ev = 0.5f;
            CHECK(mismatches(rig, rig.run(mapper, s), s, true, 2) == 0);
        }
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Tone map pass: non-finite and negative HDR texels give defined output, not NaN-poisoned or sentinel pixels")
{
    const auto device = test::make_validated_device("tonemap_nonfinite");
    {
        Rig rig(*device, rhi::Format::RGBA8_UNORM);
        ToneMapper mapper(*device, rhi::Format::RGBA8_UNORM);
        ToneMapSettings s;
        s.op = ToneMapOperator::Reinhard;
        const std::vector<uint8_t> pixels = rig.run(mapper, s);
        // Texel 11 = {NaN, 0.5, NaN}: NaN -> 0. Texel 12 = {+inf, 0.5, -inf}: +inf -> saturated 1, -inf -> 0.
        CHECK(pixels[11 * 4 + 0] == 0);
        CHECK(pixels[11 * 4 + 2] == 0);
        CHECK(pixels[12 * 4 + 0] == 255);
        CHECK(pixels[12 * 4 + 2] == 0);
        CHECK(pixels[10 * 4 + 0] == 0);  // negative -> 0
        for (uint32_t i = 0; i < kWidth; ++i) {
            CHECK(pixels[i * 4 + 3] == 255);  // every texel was written (the clear sentinel was 0.5)
        }
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Tone map pass: a floating-point target shows the exact math, at half-float precision")
{
    // An 8-bit target hides small constant errors (one level is 0.4 percent of full scale), so the operators are also checked
    // against a binary16 target, where a wrong curve constant shows as a relative error of a few thousandths.
    const auto device = test::make_validated_device("tonemap_float");
    {
        Rig rig(*device, rhi::Format::RGBA16_FLOAT);
        ToneMapper mapper(*device, rhi::Format::RGBA16_FLOAT);
        for (const ToneMapOperator op : {ToneMapOperator::Linear, ToneMapOperator::Reinhard, ToneMapOperator::AcesFitted}) {
            for (const float ev : {-1.0f, 0.0f, 1.5f}) {
                ToneMapSettings s;
                s.op = op;
                s.exposure_ev = ev;
                s.white_point = 3.0f;
                const std::vector<uint8_t> raw = rig.run(mapper, s);
                int bad = 0;
                for (uint32_t i = 0; i < kWidth; ++i) {
                    const Float3 want = tone_map(rig.reference_input[i], s);
                    const float channels[3] = {want.x, want.y, want.z};
                    for (uint32_t c = 0; c < 3; ++c) {
                        uint16_t h = 0;
                        std::memcpy(&h, &raw[(i * 4 + c) * 2], 2);
                        const float actual = from_half(h);
                        // binary16 has 11 significant bits: allow its rounding plus a hair for the shader's own float math.
                        const float tolerance = 1.5e-3f * std::max(channels[c], 0.02f) + 2e-4f;
                        if (!(std::fabs(actual - channels[c]) <= tolerance)) {
                            std::printf("  op %u ev %.1f texel %u channel %u: gpu %.6f, reference %.6f\n", static_cast<unsigned>(op),
                                        static_cast<double>(ev), i, c, static_cast<double>(actual), static_cast<double>(channels[c]));
                            ++bad;
                        }
                    }
                }
                CHECK(bad == 0);
            }
        }
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Tone map pass: invalid Reinhard white points and exposures give the same finite output as the reference")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    const auto device = test::make_validated_device("tonemap_settings");
    {
        Rig rig(*device, rhi::Format::RGBA8_UNORM);
        ToneMapper mapper(*device, rhi::Format::RGBA8_UNORM);
        for (const float white : {0.0f, -3.0f, nan, inf, 1e-30f, 1e-20f, 1e30f}) {
            for (const float ev : {0.0f, 1.0f, nan, 200.0f}) {
                ToneMapSettings s;
                s.op = ToneMapOperator::Reinhard;
                s.exposure_ev = ev;
                s.white_point = white;
                const std::vector<uint8_t> pixels = rig.run(mapper, s);
                CHECK(mismatches(rig, pixels, s, false, 1) == 0);  // identical to the sanitized reference, including the black texel
                // No sentinel survives (the target is cleared to 128) and a black texel stays black.
                CHECK(pixels[0] == 0);
                CHECK(pixels[1] == 0);
                CHECK(pixels[2] == 0);
            }
        }
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}
