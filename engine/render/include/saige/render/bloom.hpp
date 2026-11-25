// SAIGE Render — bloom: an opt-in HDR post effect, with a CPU reference of every filter and the GPU pass that runs the same chain.
//
// The scene's linear HDR color is thresholded, blurred through a chain of half-size levels and added back before tone mapping:
//
//   D_0 = downsample13(prefilter(scene))          half size; the soft-knee threshold is applied to every tap
//   D_i = downsample13(D_{i-1})                   i = 1 .. levels-1, each half the size of the one before (never below 1 x 1)
//   U_{levels-1} = D_{levels-1};  U_i = D_i + scatter * tent3x3(U_{i+1})
//   output = tone_map(scene + intensity * bilinear(U_0))
//
// Why this shape: the 13-tap downsample (Jimenez, Call of Duty: Advanced Warfare) is a wide, stable low-pass without the shimmer of a plain box; the 3x3 tent
// upsample adds the blurred coarser levels back, so the glow has a wide soft tail and no hard edge. It needs nothing but render passes and bilinear sampling
// (the RHI has no compute), and no blending into float targets, so every level is its own texture. `scatter` is how much of the wider glow carries up
// (0: only the finest blur; 1: all levels at full weight). The CPU functions below are the reference the GPU pass is tested against; they use the same
// texel-center convention as the hardware (a texel's center is at (i + 0.5) / size, clamp to edge).
//
// Opt-in and HDR-only: `RendererSettings::bloom.enabled` requires `RendererSettings::hdr` (bloom of an LDR image is meaningless: nothing exceeds 1). With it
// off nothing is created and every image is what it was before. Design and limits: docs/rendering/bloom.md.
#pragma once

#include "saige/render/math.hpp"
#include "saige/rhi/device.hpp"

#include <cstdint>
#include <vector>

namespace saige::render {

inline constexpr uint32_t kMaxBloomLevels = 8;

struct BloomSettings {
    bool enabled = false;
    float intensity = 0.05f;   ///< how much of the blurred glow is added to the scene (0 to 16); the scene is scaled by 2^EV afterwards like everything else
    float threshold = 1.0f;    ///< scene-referred brightness (largest channel) below which a pixel contributes nothing (0 or more)
    float knee = 0.5f;         ///< soft-knee width as a fraction of the threshold, 0 (hard) to 1
    float scatter = 0.7f;      ///< 0 to 1: weight of each coarser level when it is added back
    uint32_t levels = 5;       ///< downsample levels, 1 to kMaxBloomLevels (reduced automatically for tiny targets)
};

/// Throws std::invalid_argument for a non-finite value, a value outside the ranges above or zero or too many levels. (Whether bloom may be enabled is the
/// renderer's check: it needs HDR.)
void validate_bloom(const BloomSettings& settings);

/// Levels actually used for a source of this size: `requested`, reduced so that every level exists (a level is made only from a level at least 2 pixels
/// wide and high). At least 1 for a non-empty source.
[[nodiscard]] uint32_t bloom_level_count(uint32_t source_width, uint32_t source_height, uint32_t requested) noexcept;

/// Size of level `index` (0 is half the source): max(1, source >> (index + 1)).
[[nodiscard]] uint32_t bloom_level_extent(uint32_t source_extent, uint32_t index) noexcept;

// ---- CPU reference ---------------------------------------------------------------------------------------------------------------------------

/// Linear RGB, row-major, row 0 at the top (texel (0, 0) is the top-left, as the shaders' uv is).
struct BloomImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<Float3> pixels;

    BloomImage() = default;
    BloomImage(uint32_t w, uint32_t h) : width(w), height(h), pixels(std::size_t{w} * h) {}
    [[nodiscard]] Float3& at(uint32_t x, uint32_t y) { return pixels[std::size_t{y} * width + x]; }
    [[nodiscard]] const Float3& at(uint32_t x, uint32_t y) const { return pixels[std::size_t{y} * width + x]; }
};

/// Hardware-style bilinear sample at uv in [0, 1]^2 (texel centers at (i + 0.5) / size), clamp to edge.
[[nodiscard]] Float3 bloom_sample(const BloomImage& image, float u, float v) noexcept;

/// The sanitation every tap gets: NaN and negatives become 0, values above 65504 (the largest finite half float) are clamped.
[[nodiscard]] Float3 bloom_clean(Float3 c) noexcept;

/// The soft-knee threshold of one cleaned color (see bloom_down.hlsl): `knee_width` is absolute (threshold times the knee fraction).
[[nodiscard]] Float3 bloom_threshold(Float3 c, float threshold, float knee_width) noexcept;

/// One 13-tap downsample to max(1, size >> 1) in each direction; with `prefilter` set, every tap is thresholded first.
[[nodiscard]] BloomImage bloom_downsample(const BloomImage& source, bool prefilter, float threshold, float knee_width);

/// fine + scatter * tent3x3(coarse): the result has the size of `fine`.
[[nodiscard]] BloomImage bloom_combine(const BloomImage& fine, const BloomImage& coarse, float scatter);

/// The whole chain: returns U_0 (half the size of `scene`). `settings` must be valid.
[[nodiscard]] BloomImage bloom_chain(const BloomImage& scene, const BloomSettings& settings);

/// What the tone-map pass sees for a pixel: scene + intensity * bloom, before exposure and the operator.
[[nodiscard]] Float3 bloom_composite(Float3 scene, Float3 bloom, float intensity) noexcept;

// ---- GPU pass --------------------------------------------------------------------------------------------------------------------------------

/// The GPU objects of one chain for one source size: the down and up textures (RGBA16_FLOAT, color attachment and sampled) and the bind groups of every pass.
/// Plain handles, owned by the caller through BloomPass::create_chain / destroy_chain.
struct BloomChain {
    uint32_t levels = 0;
    std::vector<rhi::TextureHandle> down;            ///< D_0 .. D_{levels-1}
    std::vector<rhi::TextureHandle> up;              ///< U_0 .. U_{levels-2}; U_{levels-1} is D_{levels-1}
    std::vector<rhi::BindGroupHandle> down_groups;   ///< level i reads the source (i = 0) or D_{i-1}
    std::vector<rhi::BindGroupHandle> up_groups;     ///< level i reads D_i and U_{i+1} (or D_{levels-1})
    uint32_t source_width = 0;
    uint32_t source_height = 0;

    [[nodiscard]] bool valid() const noexcept { return levels != 0; }
    /// U_0: what the tone-map pass adds.
    [[nodiscard]] rhi::TextureHandle result() const noexcept { return levels > 1 ? up[0] : down[0]; }
    /// The texture U_i (the last level's U is its D).
    [[nodiscard]] rhi::TextureHandle up_texture(uint32_t i) const noexcept { return i + 1 < levels ? up[i] : down[i]; }
};

/// The bloom pipelines (prefilter-downsample, downsample, upsample) and the linear sampler, for one `rhi::Format::RGBA16_FLOAT` chain format. Owns what it
/// creates and releases it on destruction (destroy it before the device); a failed constructor releases everything it had created.
class BloomPass {
public:
    BloomPass(rhi::Device& device, const BloomSettings& settings);
    ~BloomPass();
    BloomPass(const BloomPass&) = delete;
    BloomPass& operator=(const BloomPass&) = delete;

    /// Creates a chain for a sampled floating-point source texture of `width` x `height`. All or nothing: on a device failure everything created is released and the
    /// exception propagates.
    /// `readable` adds TransferSrc usage to the chain's textures so a test or a debug tool can copy a level out; the renderer leaves it off.
    [[nodiscard]] BloomChain create_chain(rhi::TextureHandle source, uint32_t width, uint32_t height, bool readable = false);
    /// Releases a chain's textures and bind groups on the device's deferred timeline. Safe on an empty chain; leaves it empty.
    void destroy_chain(BloomChain& chain) noexcept;

    /// The description of D_level / U_level for a source of this size (also what the render graph imports them with).
    [[nodiscard]] static rhi::TextureDesc level_desc(uint32_t source_width, uint32_t source_height, uint32_t level);
    /// Records the draw that writes D_i (a complete rendering: begin, viewport and scissor, draw, end). The source and the previous level must be sampled.
    void record_down(rhi::CommandList& cmd, const BloomChain& chain, uint32_t level) const;
    /// Records the draw that writes U_i, i in [0, levels-2].
    void record_up(rhi::CommandList& cmd, const BloomChain& chain, uint32_t level) const;
    [[nodiscard]] const BloomSettings& settings() const noexcept { return settings_; }

private:
    void release() noexcept;

    rhi::Device& device_;
    BloomSettings settings_;
    rhi::BindGroupLayoutHandle down_layout_, up_layout_;
    rhi::ShaderHandle vs_down_, ps_prefilter_, ps_down_, vs_up_, ps_up_;
    rhi::PipelineHandle prefilter_pipeline_, down_pipeline_, up_pipeline_;
    rhi::SamplerHandle sampler_;
};

} // namespace saige::render
