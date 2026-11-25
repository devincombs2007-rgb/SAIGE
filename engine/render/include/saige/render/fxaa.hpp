// SAIGE Render — FXAA: an opt-in post-process anti-aliasing pass, with a CPU reference and the full-screen pass that runs the same algorithm.
//
// FXAA smooths the aliased edges of the finished image: a luma contrast test (a pixel whose neighbourhood is flat is copied exactly), then a blend along the edge direction the
// luma gradient implies (shaders/scene/fxaa.hlsl; Timothy Lottes' compact form). It needs no geometry, depth or extra samples of the scene, so it works with any content and costs
// one full-screen pass; it cannot recover sub-pixel detail that was never sampled (thin wires that skip pixels), and it slightly softens text and fine texture, which is why it is
// opt-in. `RendererSettings::fxaa` enables it: the scene (and the tone map, in HDR) then renders into an intermediate color target of the output's format and the pass writes
// the output. Off by default: nothing is created and every image is what it was before. Details and limits: docs/rendering/fxaa.md.
#pragma once

#include "saige/render/math.hpp"
#include "saige/rhi/device.hpp"

#include <cstdint>
#include <vector>

namespace saige::render {

struct FxaaSettings {
    bool enabled = false;
    float edge_threshold = 0.125f;       ///< 0 to 1: the luma range, relative to the brightest neighbour, below which a pixel is left alone (lower = more edges smoothed)
    float edge_threshold_min = 0.0312f;  ///< 0 to 1: the absolute luma range below which a pixel is left alone (keeps dark noise out)
    float span_max = 8.0f;               ///< 1 to 32: the longest blend along an edge, in texels
};

/// Throws std::invalid_argument for a non-finite value or one outside the ranges above.
void validate_fxaa(const FxaaSettings& settings);

/// Linear or display-referred RGB, row-major, row 0 at the top.
struct FxaaImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<Float3> pixels;

    FxaaImage() = default;
    FxaaImage(uint32_t w, uint32_t h) : width(w), height(h), pixels(std::size_t{w} * h) {}
    [[nodiscard]] Float3& at(uint32_t x, uint32_t y) { return pixels[std::size_t{y} * width + x]; }
    [[nodiscard]] const Float3& at(uint32_t x, uint32_t y) const { return pixels[std::size_t{y} * width + x]; }
};

/// The perceptual luma the pass tests: `color` raised to `luma_exponent` (0.5 when the values are linear, 1 when they already are display values), weighted 0.299, 0.587, 0.114.
[[nodiscard]] float fxaa_luma(Float3 color, float luma_exponent) noexcept;

/// The reference: exactly the shader's algorithm with hardware-style bilinear sampling (texel centers at (i + 0.5) / size, clamp to edge). `settings` must be valid.
[[nodiscard]] FxaaImage fxaa_apply(const FxaaImage& source, const FxaaSettings& settings, float luma_exponent);

/// The pass. Records one draw into a rendering whose single color attachment has the format given at construction (the format of the texture it reads: the intermediate target is
/// created in the output's format, so the pass reads and writes the same kind of values). Owns its shaders, pipeline, layout and sampler and releases them on destruction
/// (destroy it before the device); a failed constructor releases whatever it had created.
class FxaaPass {
public:
    FxaaPass(rhi::Device& device, rhi::Format target_format);
    ~FxaaPass();
    FxaaPass(const FxaaPass&) = delete;
    FxaaPass& operator=(const FxaaPass&) = delete;

    /// A bind group sampling `color` (a 2D texture with Sampled usage, in `target_format`). The caller owns it and destroys it with the device once no frame uses it.
    [[nodiscard]] rhi::BindGroupHandle create_input(rhi::TextureHandle color);
    /// Binds the pipeline and `input` and draws the triangle; viewport and scissor are the caller's. `width` and `height` are the source texture's size.
    void draw(rhi::CommandList& cmd, rhi::BindGroupHandle input, const FxaaSettings& settings, uint32_t width, uint32_t height);
    /// The luma exponent this pass uses (0.5 for an sRGB format whose sampler decodes to linear, else 1).
    [[nodiscard]] float luma_exponent() const noexcept { return luma_exponent_; }

private:
    void release() noexcept;

    rhi::Device& device_;
    float luma_exponent_ = 1.0f;
    rhi::BindGroupLayoutHandle layout_;
    rhi::ShaderHandle vs_, ps_;
    rhi::PipelineHandle pipeline_;
    rhi::SamplerHandle sampler_;
};

} // namespace saige::render
