// SAIGE Render — ambient occlusion: an opt-in, depth-only screen-space pass with a CPU reference and the GPU passes that run the same algorithm.
//
// For every pixel the pass rebuilds the view-space position from the scene depth (reverse-Z infinite: view distance = near / depth), the surface normal from the neighbouring
// depths, and compares twelve taps spread over a disc of `radius` meters around it, in the manner of Alchemy's ambient obscurance: a tap that lies in front of the surface plane
// and close to the pixel occludes it, in proportion to how directly above the surface it sits (shaders/scene/ao_generate.hlsl). The 4x4 pattern of rotations the taps use is then
// removed by a 5x5 depth-aware blur that is folded into the pass that applies the occlusion to the HDR scene color (shaders/scene/ao_apply.hlsl). The CPU functions below are step
// for step the same arithmetic.
//
// What it is and is not. The scene is lit in one forward pass, so this occlusion multiplies the finished HDR color: it darkens creases and contact points in everything, including
// directly lit surfaces, which a real ambient-only term would leave alone. It is a screen-space effect: it sees only what the depth buffer holds (no off-screen or hidden
// geometry; a thin object in front of a surface darkens it as if it were a wall within `radius`). It needs `RendererSettings::hdr`. Off by default: nothing is created and every
// image is what it was before. Details and limits: docs/rendering/ambient-occlusion.md.
#pragma once

#include "saige/render/math.hpp"
#include "saige/rhi/device.hpp"

#include <cstdint>
#include <vector>

namespace saige::render {

struct AmbientOcclusionSettings {
    bool enabled = false;
    float radius = 0.75f;     ///< meters, in [0.01, 16]: how far from a surface geometry can occlude it
    float intensity = 1.0f;   ///< in [0, 4]: 0 is no occlusion
    float bias = 0.05f;       ///< in [0, 0.5], as a fraction of `radius`: how far above the surface plane a tap must be before it counts (keeps flat surfaces clean)
};

/// Throws std::invalid_argument for a non-finite value or one outside the ranges above.
void validate_ambient_occlusion(const AmbientOcclusionSettings& settings);

inline constexpr uint32_t ao_sample_count = 12;  ///< taps per pixel
inline constexpr int ao_blur_radius = 2;         ///< the apply pass blurs over (2r + 1)^2 pixels
inline constexpr float ao_max_radius_pixels = 48.0f;

/// What the passes need to know about the view that produced the depth.
struct AoView {
    uint32_t width = 0;
    uint32_t height = 0;
    float near_plane = 0.1f;
    float tan_half_fov_y = 0.5773503f;  ///< tan(fov_y / 2); the horizontal extent follows from the aspect ratio
};

/// Reverse-Z device depth, row-major, row 0 at the top; 0 means "nothing there" (the sky).
struct AoDepthImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> depth;

    AoDepthImage() = default;
    AoDepthImage(uint32_t w, uint32_t h) : width(w), height(h), depth(std::size_t{w} * h, 0.0f) {}
    [[nodiscard]] float& at(uint32_t x, uint32_t y) { return depth[std::size_t{y} * width + x]; }
    [[nodiscard]] float at(uint32_t x, uint32_t y) const { return depth[std::size_t{y} * width + x]; }
};

/// Per-pixel occlusion in [0, 1] (1 = open), the generate pass.
struct AoImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<float> value;

    AoImage() = default;
    AoImage(uint32_t w, uint32_t h) : width(w), height(h), value(std::size_t{w} * h, 1.0f) {}
    [[nodiscard]] float& at(uint32_t x, uint32_t y) { return value[std::size_t{y} * width + x]; }
    [[nodiscard]] float at(uint32_t x, uint32_t y) const { return value[std::size_t{y} * width + x]; }
};

/// The generate pass on the CPU. `settings` must be valid, the depth must match `view`'s size.
[[nodiscard]] AoImage ao_generate(const AoDepthImage& depth, const AoView& view, const AmbientOcclusionSettings& settings);
/// The depth-aware 5x5 blur of `ao`, per pixel (the sky and pixels across a depth edge do not contribute).
[[nodiscard]] AoImage ao_blur(const AoImage& ao, const AoDepthImage& depth, const AoView& view, const AmbientOcclusionSettings& settings);

/// The two passes, with the push constants they share. Owns its shaders, pipelines and layouts and releases them on destruction (destroy it before the device); a failed
/// constructor releases whatever it had created. The occlusion target is a single-channel half-float texture ("AoValue", `ao_format`).
class AmbientOcclusionPass {
public:
    static constexpr rhi::Format ao_format = rhi::Format::R16_FLOAT;

    explicit AmbientOcclusionPass(rhi::Device& device);
    ~AmbientOcclusionPass();
    AmbientOcclusionPass(const AmbientOcclusionPass&) = delete;
    AmbientOcclusionPass& operator=(const AmbientOcclusionPass&) = delete;

    /// Bind group for the generate pass: `depth` (a depth texture with Sampled usage).
    [[nodiscard]] rhi::BindGroupHandle create_generate_input(rhi::TextureHandle depth);
    /// Bind group for the apply pass: the HDR scene color, the occlusion texture and the depth. The caller owns both groups and destroys them with the device.
    [[nodiscard]] rhi::BindGroupHandle create_apply_input(rhi::TextureHandle color, rhi::TextureHandle ao, rhi::TextureHandle depth);

    /// Each binds its pipeline and input and draws the triangle; viewport and scissor are the caller's. The generate pass renders into an `ao_format` attachment, the apply pass
    /// into the HDR format given for `apply_format`.
    void draw_generate(rhi::CommandList& cmd, rhi::BindGroupHandle input, const AoView& view, const AmbientOcclusionSettings& settings);
    void draw_apply(rhi::CommandList& cmd, rhi::BindGroupHandle input, const AoView& view, const AmbientOcclusionSettings& settings);

    void set_apply_format(rhi::Format format);  ///< creates the apply pipeline for the HDR color format (call once, before draw_apply)

private:
    void release() noexcept;

    rhi::Device& device_;
    rhi::BindGroupLayoutHandle generate_layout_, apply_layout_;
    rhi::ShaderHandle vs_, generate_ps_, apply_ps_;
    rhi::PipelineHandle generate_pipeline_, apply_pipeline_;
};

} // namespace saige::render
