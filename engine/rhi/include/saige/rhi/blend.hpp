// SAIGE RHI — color blend state of a graphics pipeline.
//
// Optional and backward compatible: `GraphicsPipelineDesc::blend` is empty by default, which means blending is disabled for every
// color attachment (output overwrites the target, as before this header existed). When present it has one entry per color
// attachment.
//
// Portable subset, so every backend can honor it:
//  * the same blend state for every color attachment (independent per-attachment blending is not exposed; the renderer draws to
//    one color attachment);
//  * blending only into formats that blend on every Vulkan implementation: UNORM, sRGB and 16-bit float color formats (and
//    packed RG11B10 float), not integer, SNORM, 32-bit float, shared-exponent or block-compressed formats;
//  * the factors and operations below (no dual-source blending and no constant blend color yet; both extend the enums later).
//
// Blending happens in the target's native space: into an *_SRGB target the hardware decodes the destination to linear, blends
// linearly and encodes the result, so source-over of linear colors is correct there.
#pragma once

#include "saige/rhi/format.hpp"
#include "saige/rhi/resource_desc.hpp"

#include <cstdint>
#include <span>

namespace saige::rhi {

enum class BlendFactor : uint8_t {
    Zero,
    One,
    SrcColor,
    OneMinusSrcColor,
    DstColor,
    OneMinusDstColor,
    SrcAlpha,
    OneMinusSrcAlpha,
    DstAlpha,
    OneMinusDstAlpha,
    Count
};

enum class BlendOp : uint8_t {
    Add,              ///< src * sf + dst * df
    Subtract,         ///< src * sf - dst * df
    ReverseSubtract,  ///< dst * df - src * sf
    Min,              ///< min(src, dst); the factors are ignored
    Max,              ///< max(src, dst); the factors are ignored
    Count
};

struct BlendState {
    bool enable = false;
    BlendFactor src_color = BlendFactor::One;
    BlendFactor dst_color = BlendFactor::Zero;
    BlendOp color_op = BlendOp::Add;
    BlendFactor src_alpha = BlendFactor::One;
    BlendFactor dst_alpha = BlendFactor::Zero;
    BlendOp alpha_op = BlendOp::Add;

    friend constexpr bool operator==(const BlendState&, const BlendState&) noexcept = default;

    /// Straight (non-premultiplied) alpha, source-over: color = src.rgb * src.a + dst.rgb * (1 - src.a), alpha = src.a + dst.a * (1 - src.a).
    /// This is glTF's `BLEND` alpha mode.
    [[nodiscard]] static constexpr BlendState source_over() noexcept
    {
        return {true, BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One,
                BlendFactor::OneMinusSrcAlpha, BlendOp::Add};
    }
    /// Premultiplied alpha, source-over: out = src + dst * (1 - src.a), for color and alpha.
    [[nodiscard]] static constexpr BlendState premultiplied_over() noexcept
    {
        return {true, BlendFactor::One, BlendFactor::OneMinusSrcAlpha, BlendOp::Add, BlendFactor::One, BlendFactor::OneMinusSrcAlpha,
                BlendOp::Add};
    }
    /// out = src + dst (saturating in a UNORM target).
    [[nodiscard]] static constexpr BlendState additive() noexcept
    {
        return {true, BlendFactor::One, BlendFactor::One, BlendOp::Add, BlendFactor::One, BlendFactor::One, BlendOp::Add};
    }
};

/// Whether `format` is in the portable subset that can be blended into (see the top of this header).
[[nodiscard]] bool is_blendable_color_format(Format format) noexcept;

/// Checks a pipeline's blend states against its color attachment formats. An empty `blend` is valid (blending disabled). Otherwise
/// there must be exactly one entry per color attachment, all equal, every factor and operation in range, and every attachment that
/// blends must have a blendable format. Returns the first problem found.
[[nodiscard]] ValidationResult validate_blend_state(std::span<const BlendState> blend, std::span<const Format> color_formats) noexcept;

} // namespace saige::rhi
