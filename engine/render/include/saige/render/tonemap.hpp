// HDR tone mapping: a CPU reference of the shader's math, and the full-screen pass that applies it.
//
// The scene renders in linear light into a floating-point target; this pass turns that into a displayable linear color for the
// output target (whose format does the sRGB encoding). It is independent of the scene renderer: the 0.2 path (sRGB target, no tone
// map) is unchanged, and the pass can be tested by itself against `tone_map`. Design: docs/rendering/material-architecture.md
// section 4.
#pragma once

#include "saige/render/math.hpp"
#include "saige/rhi/device.hpp"

#include <cstdint>

namespace saige::render {

enum class ToneMapOperator : uint32_t {
    Linear = 0,      ///< saturate: identity in [0, 1], clips above (the 0.2 look)
    Reinhard = 1,    ///< extended Reinhard, saturated: c (1 + c / w^2) / (1 + c); the white point w maps to 1
    AcesFitted = 2,  ///< Narkowicz's fit of the ACES curve
};

/// The default operator is the ACES fit (owner decision for the 0.3 HDR path); Linear stays available for the LDR look and other
/// operators can be added to the enum without changing callers. The 0.2 renderer does not use this pass at all.
struct ToneMapSettings {
    ToneMapOperator op = ToneMapOperator::AcesFitted;
    float exposure_ev = 0.0f;  ///< exposure in stops; the scene color is scaled by 2^EV before the operator
    float white_point = 4.0f;  ///< Reinhard only: the scene luminance that maps to white (must be positive)
};

[[nodiscard]] float exposure_scale(float exposure_ev) noexcept;

/// The reference: exactly the shader's math, including its handling of negative and non-finite input (NaN and negatives become
/// 0, values above 65504, the largest finite half float, are clamped). Returns linear color in [0, 1].
[[nodiscard]] Float3 tone_map(Float3 linear_hdr, const ToneMapSettings& settings) noexcept;

/// The full-screen pass. Records one draw inside a rendering whose single color attachment has the format given at construction;
/// the HDR input is a sampled texture wrapped in a bind group made by `create_input`. The mapper owns its shaders, pipeline and
/// sampler and releases them on destruction (destroy it before the device).
class ToneMapper {
public:
    ToneMapper(rhi::Device& device, rhi::Format target_format);
    ~ToneMapper();
    ToneMapper(const ToneMapper&) = delete;
    ToneMapper& operator=(const ToneMapper&) = delete;

    /// A bind group sampling `hdr` (a 2D texture with Sampled usage, in a floating-point or other filterable format). The caller
    /// owns it and destroys it with the device once no frame uses it.
    [[nodiscard]] rhi::BindGroupHandle create_input(rhi::TextureHandle hdr);

    /// Binds the pipeline and `input` and draws the triangle. Viewport and scissor are the caller's (they set the output area).
    void draw(rhi::CommandList& cmd, rhi::BindGroupHandle input, const ToneMapSettings& settings);

private:
    rhi::Device& device_;
    rhi::BindGroupLayoutHandle layout_;
    rhi::ShaderHandle vs_;
    rhi::ShaderHandle ps_;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_;
};

} // namespace saige::render
