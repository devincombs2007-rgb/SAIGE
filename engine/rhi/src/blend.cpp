#include "saige/rhi/blend.hpp"

namespace saige::rhi {

bool is_blendable_color_format(Format format) noexcept
{
    // The formats for which the Vulkan specification makes VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT mandatory for optimal tiling
    // (the "Mandatory Format Support" tables of the Formats chapter, checked against the specification source): R8, RG8, RGBA8 and BGRA8
    // in UNORM and sRGB, A2B10G10R10 UNORM, and the 16-bit float formats R16, RG16 and RGBA16. Nothing else is promised on every
    // implementation: not SNORM or integer formats, not 16-bit UNORM, not packed RG11B10 float, not 32-bit float, not shared-exponent
    // or compressed formats. (A device may blend into more; the backend additionally checks the selected device for the formats it
    // is given.)
    switch (format) {
    case Format::R8_UNORM:
    case Format::RG8_UNORM:
    case Format::RGBA8_UNORM:
    case Format::RGBA8_SRGB:
    case Format::BGRA8_UNORM:
    case Format::BGRA8_SRGB:
    case Format::RGB10A2_UNORM:
    case Format::R16_FLOAT:
    case Format::RG16_FLOAT:
    case Format::RGBA16_FLOAT:
        return true;
    default:
        return false;
    }
}

ValidationResult validate_blend_state(std::span<const BlendState> blend, std::span<const Format> color_formats) noexcept
{
    if (blend.empty()) {
        return {};
    }
    if (blend.size() != color_formats.size()) {
        return {"blend states must be empty or have one entry per color attachment"};
    }
    for (const BlendState& b : blend) {
        if (b.src_color >= BlendFactor::Count || b.dst_color >= BlendFactor::Count || b.src_alpha >= BlendFactor::Count ||
            b.dst_alpha >= BlendFactor::Count) {
            return {"blend factor out of range"};
        }
        if (b.color_op >= BlendOp::Count || b.alpha_op >= BlendOp::Count) {
            return {"blend operation out of range"};
        }
        if (!(b == blend.front())) {
            return {"independent blend state per color attachment is not supported: every attachment must use the same state"};
        }
    }
    if (blend.front().enable) {
        for (const Format f : color_formats) {
            if (!is_blendable_color_format(f)) {
                return {"blending into this color format is not guaranteed by the Vulkan specification (portable subset: R8, RG8, RGBA8, BGRA8 UNORM or sRGB, RGB10A2 UNORM, R16, RG16, RGBA16 float)"};
            }
        }
    }
    return {};
}

} // namespace saige::rhi
