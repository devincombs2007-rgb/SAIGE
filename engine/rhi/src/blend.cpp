#include "saige/rhi/blend.hpp"

namespace saige::rhi {

bool is_blendable_color_format(Format format) noexcept
{
    if (format == Format::Unknown || is_compressed(format) || is_depth_or_stencil(format)) {
        return false;
    }
    switch (format) {
    case Format::R32_FLOAT:
    case Format::RG32_FLOAT:
    case Format::RGB32_FLOAT:
    case Format::RGBA32_FLOAT:
    case Format::RGB9E5_UFLOAT:  // not renderable at all
        return false;
    default:
        break;
    }
    switch (get_format_info(format).type) {
    case FormatType::Unorm:
    case FormatType::UnormSrgb:
    case FormatType::Float:
        return true;
    default:
        return false;  // integer and SNORM formats
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
                return {"blending into this color format is not in the portable subset (UNORM, sRGB and 16-bit float formats)"};
            }
        }
    }
    return {};
}

} // namespace saige::rhi
