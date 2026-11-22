#include "saige/gfx_test/saige_test.hpp"

#include "saige/rhi/blend.hpp"
#include "saige/rhi/device.hpp"

#include <array>

using namespace saige::rhi;

TEST_CASE("Blend: the default is disabled, and a pipeline description without blend states means no blending")
{
    const BlendState d;
    CHECK_FALSE(d.enable);
    CHECK(d.src_color == BlendFactor::One);
    CHECK(d.dst_color == BlendFactor::Zero);  // One / Zero with Add overwrites, even if someone enables it
    CHECK(d.color_op == BlendOp::Add);
    const GraphicsPipelineDesc desc;
    CHECK(desc.blend.empty());  // backward compatible: existing descriptions are untouched
    const std::array<Format, 1> formats{Format::RGBA8_SRGB};
    CHECK(validate_blend_state(desc.blend, formats).ok());
}

TEST_CASE("Blend: the presets are the textbook equations")
{
    const BlendState a = BlendState::source_over();
    CHECK(a.enable);
    CHECK(a.src_color == BlendFactor::SrcAlpha);
    CHECK(a.dst_color == BlendFactor::OneMinusSrcAlpha);
    CHECK(a.src_alpha == BlendFactor::One);             // alpha = a + dst.a * (1 - a)
    CHECK(a.dst_alpha == BlendFactor::OneMinusSrcAlpha);
    CHECK(a.color_op == BlendOp::Add);
    const BlendState p = BlendState::premultiplied_over();
    CHECK(p.src_color == BlendFactor::One);
    CHECK(p.dst_color == BlendFactor::OneMinusSrcAlpha);
    const BlendState s = BlendState::additive();
    CHECK(s.src_color == BlendFactor::One);
    CHECK(s.dst_color == BlendFactor::One);
    CHECK(a != p);
    CHECK(a == BlendState::source_over());
}

TEST_CASE("Blend: validation accepts one equal state per attachment into blendable formats")
{
    const std::array<Format, 2> two{Format::RGBA8_UNORM, Format::RGBA16_FLOAT};
    const std::array<BlendState, 2> same{BlendState::source_over(), BlendState::source_over()};
    CHECK(validate_blend_state(same, two).ok());
    const std::array<BlendState, 1> one{BlendState::additive()};
    for (const Format f : {Format::RGBA8_UNORM, Format::RGBA8_SRGB, Format::BGRA8_UNORM, Format::BGRA8_SRGB, Format::RGBA16_FLOAT,
                           Format::RG16_FLOAT, Format::R16_FLOAT, Format::RG11B10_FLOAT, Format::RGB10A2_UNORM}) {
        const std::array<Format, 1> formats{f};
        CHECK(validate_blend_state(one, formats).ok());
        CHECK(is_blendable_color_format(f));
    }
}

TEST_CASE("Blend: validation rejects what no backend can honor portably")
{
    const std::array<BlendState, 1> blend{BlendState::source_over()};
    // Formats outside the portable subset.
    for (const Format f : {Format::RGBA8_UINT, Format::R32_SINT, Format::RGBA8_SNORM, Format::RGBA32_FLOAT, Format::R32_FLOAT,
                           Format::RGB9E5_UFLOAT, Format::D32_FLOAT, Format::BC7_UNORM, Format::Unknown}) {
        const std::array<Format, 1> formats{f};
        CHECK_FALSE(validate_blend_state(blend, formats).ok());
        CHECK_FALSE(is_blendable_color_format(f));
    }
    // A disabled state never needs a blendable format: integer targets keep working.
    const std::array<BlendState, 1> disabled{BlendState{}};
    const std::array<Format, 1> integer{Format::RGBA8_UINT};
    CHECK(validate_blend_state(disabled, integer).ok());

    // Count mismatch and independent per-attachment states.
    const std::array<Format, 2> two{Format::RGBA8_UNORM, Format::RGBA8_UNORM};
    CHECK_FALSE(validate_blend_state(blend, two).ok());
    const std::array<BlendState, 2> different{BlendState::source_over(), BlendState::additive()};
    CHECK_FALSE(validate_blend_state(different, two).ok());

    // Out-of-range enumerators.
    BlendState bad = BlendState::source_over();
    bad.src_color = BlendFactor::Count;
    const std::array<BlendState, 1> bad_factor{bad};
    const std::array<Format, 1> rgba{Format::RGBA8_UNORM};
    CHECK_FALSE(validate_blend_state(bad_factor, rgba).ok());
    bad = BlendState::source_over();
    bad.alpha_op = BlendOp::Count;
    const std::array<BlendState, 1> bad_op{bad};
    CHECK_FALSE(validate_blend_state(bad_op, rgba).ok());
}
