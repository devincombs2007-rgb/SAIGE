// SAIGE Render — draws GpuModels inside a scene pass.
//
// Owned by SceneRenderer (created on first use) and recorded into its pass, so imported models share the depth buffer with
// the ground and the 0.1 cubes. Pipelines come in four variants (front face CCW or CW, culling back faces or none): the
// renderer picks the clockwise variant for an instance whose world matrix mirrors (negative determinant), so mirrored
// transforms are well-defined. Instances whose matrix is not finite, or whose determinant is ~zero, are skipped and counted.
//
// A material has five texture slots (base color, metallic-roughness, normal, occlusion, emissive), each with its own
// sampler, plus a parameter block (docs/rendering/material-architecture.md). Slots a material leaves empty are bound to
// neutral defaults, so one shader serves every material. Shaded: base color, emissive and occlusion (occlusion scales the
// ambient term, as glTF defines it) in both modes; metallic-roughness and tangent-space normal mapping (with the material's
// normal scale and the vertex tangents) only in `ModelShading::MetallicRoughness`. Alpha-mask materials discard below their cutoff; alpha
// blending is not implemented (a Blend material draws as opaque).
#pragma once

#include "saige/render/gpu_resources.hpp"
#include "saige/render/math.hpp"
#include "saige/render/model.hpp"
#include "saige/rhi/device.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace saige::render {

/// The texture slots of a material, in binding order. The color space is a property of the slot: a texture bound to
/// BaseColor or Emissive must be sRGB-encoded (a *_SRGB format), the others linear (*_UNORM).
enum class MaterialSlot : uint8_t { BaseColor, MetallicRoughness, Normal, Occlusion, Emissive };
inline constexpr uint32_t kMaterialSlotCount = 5;

/// What an asset says about a material: factors and state, no GPU objects. glTF defaults throughout.
struct MaterialData {
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  ///< linear factor
    float emissive[3] = {0.0f, 0.0f, 0.0f};          ///< linear, may exceed 1
    float alpha_cutoff = 0.0f;                       ///< > 0 discards fragments below it (alpha mask); 0 = opaque
    float metallic = 1.0f;                           ///< factor, multiplies the texture's B (MetallicRoughness shading)
    float roughness = 1.0f;                          ///< factor, multiplies the texture's G (MetallicRoughness shading)
    float normal_scale = 1.0f;                       ///< scales the normal texture's x and y (MetallicRoughness shading)
    float occlusion_strength = 1.0f;                 ///< 0 ignores the occlusion texture, 1 applies it fully
    bool double_sided = false;
};
/// The 0.2 name, kept so existing callers compile.
using MaterialParams = MaterialData;

/// One slot's texture and the sampler that texture names. Either may be empty: empty means the slot's neutral default
/// texture and the renderer's default trilinear/repeat sampler.
struct MaterialTextureSlot {
    TextureRef texture;
    SamplerRef sampler;
};

struct MaterialTextures {
    MaterialTextureSlot slots[kMaterialSlotCount];
    [[nodiscard]] MaterialTextureSlot& operator[](MaterialSlot s) noexcept { return slots[static_cast<uint32_t>(s)]; }
    [[nodiscard]] const MaterialTextureSlot& operator[](MaterialSlot s) const noexcept { return slots[static_cast<uint32_t>(s)]; }
};

struct ModelFrameStats {
    uint32_t draw_calls = 0;
    uint32_t instances_drawn = 0;
    uint32_t instances_skipped = 0;  ///< non-finite or degenerate world matrix
};

/// Renderer-local lighting selection. These values also identify the shader modes.
enum class ModelShading : uint8_t {
    Legacy = 0,             ///< SAIGE 0.2 lighting and base-color-only staging.
    Extended = 1,           ///< Legacy lighting with occlusion and emissive textures.
    MetallicRoughness = 2,  ///< glTF physically based shading.
};

/// Sun shadows for imported models (SceneRenderer drives them from RendererSettings::shadows). The shadow map always exists so the
/// shader's bindings are always valid; with shadows off it is a single texel that is never sampled.
struct ModelShadowSettings {
    bool enabled = false;
    uint32_t map_size = 2048;           ///< texels per side, 16 to 8192
    float depth_bias_constant = -1024.0f;  ///< rasterizer bias of the shadow pass (rhi::DepthBias); negative moves casters away from the sun
    float depth_bias_slope = -1.5f;
};

/// Per-frame input of the sun shadow lookup: the light's orthographic view-projection (reverse-Z, x and y in [-1, 1], depth in [0, 1])
/// and how to filter.
struct ModelShadowFrame {
    Mat4 light_view_proj = Mat4::identity();
    float texel_world_size = 0.0f;     ///< meters per shadow-map texel
    float normal_offset_texels = 1.5f; ///< receivers are moved along their geometric normal by this many texels before the lookup
    uint32_t filter_radius = 1;        ///< PCF radius in texels: (2r + 1)^2 comparison taps
};

struct ModelRenderSettings {
    ModelShadowSettings shadows;
    ModelShading shading = ModelShading::Legacy;
    rhi::ClearColor sky{0.30f, 0.50f, 0.82f, 1.0f};  ///< also the fog color (linear)
    float fog_density = 0.012f;                      ///< per meter
};

class ModelRenderer {
public:
    /// Creates the shaders, the four pipeline variants, the material layout, the immutable scene constants and a white
    /// fallback texture. All GPU objects go through `registry` or are released by the destructor. Throws on device failure.
    ModelRenderer(rhi::Device& device, GpuResourceRegistry& registry, rhi::Format color_format, rhi::Format depth_format,
                  const ModelRenderSettings& settings);
    ~ModelRenderer();
    ModelRenderer(const ModelRenderer&) = delete;
    ModelRenderer& operator=(const ModelRenderer&) = delete;

    /// Builds a material: a bind group with the five textures (neutral defaults for empty slots), the five samplers
    /// (the default trilinear/repeat sampler for empty ones) and a uniform block of the parameters.
    [[nodiscard]] GpuMaterial create_material(const MaterialData& data, const MaterialTextures& textures, const char* name);

    /// The 0.2 form: a base color texture and its sampler, every other slot neutral.
    [[nodiscard]] GpuMaterial create_material(const MaterialData& data, const TextureRef& base_color, const SamplerRef& sampler,
                                              const char* name);

    /// Records the draws into an active rendering pass whose color and depth formats are the ones given at construction.
    void draw(rhi::CommandList& cmd, const Mat4& view_proj, Float3 camera_position, std::span<const ModelDraw> draws,
              ModelFrameStats& stats);

    /// GPU textures the renderer itself keeps alive: three neutral slot textures, the environment BRDF table and the sun shadow map
    /// (visible in the registry's texture counters).
    static constexpr uint32_t kOwnedTextureCount = 5;

    /// Unit vector toward the sun, the direction the lighting uses (and the shadow pass looks along, reversed).
    [[nodiscard]] Float3 sun_direction() const noexcept { return sun_direction_; }
    [[nodiscard]] bool shadows_enabled() const noexcept { return shadows_enabled_; }
    /// The sun shadow map: a D32_FLOAT texture (1x1 when shadows are off). The caller (SceneRenderer's render graph) owns its
    /// synchronization: it must be written by the shadow pass and in `rhi::sampled_texture_state` while the scene pass draws.
    [[nodiscard]] rhi::TextureHandle shadow_map() const noexcept { return shadow_map_.handle(); }
    [[nodiscard]] uint32_t shadow_map_size() const noexcept { return shadow_map_size_; }
    /// Selects this frame's shadow block for draw(): call once per frame, between begin_frame and end_frame. `nullptr` means no
    /// shadows this frame (the sun is never occluded). The block lives in a ring of `DeviceInfo::frames_in_flight` buffers indexed by
    /// the device's frame number, so rewriting it never races the GPU's reads of an earlier frame.
    void set_shadow_frame(const ModelShadowFrame* frame);
    /// Records the shadow pass's draws into an active depth-only rendering pass (the shadow map as its depth attachment, viewport and
    /// scissor set by the caller): every primitive of every draw is rendered depth only, without materials (alpha-masked materials
    /// therefore cast as if opaque). Throws std::logic_error if shadows are off.
    void draw_shadow(rhi::CommandList& cmd, const Mat4& light_view_proj, std::span<const ModelDraw> draws, ModelFrameStats& stats);

    [[nodiscard]] ModelShading shading() const noexcept { return shading_; }
    [[nodiscard]] rhi::BindGroupLayoutHandle material_layout() const noexcept { return material_layout_; }

private:
    [[nodiscard]] rhi::PipelineHandle pipeline_for(bool mirrored, bool double_sided) const noexcept;

    rhi::Device& device_;
    GpuResourceRegistry& registry_;
    ModelShading shading_;
    rhi::ShaderHandle vs_, ps_, shadow_vs_;
    rhi::BindGroupLayoutHandle material_layout_, constants_layout_;
    rhi::PipelineHandle pipelines_[4];  ///< [mirrored][double_sided]
    rhi::PipelineHandle shadow_pipeline_;  ///< depth only; invalid when shadows are off
    std::vector<BindGroupRef> constants_groups_;  ///< set 1 per frame slot: identical but for the shadow block
    std::vector<BufferRef> shadow_blocks_;
    uint32_t active_group_ = 0;
    TextureRef shadow_map_;
    SamplerRef shadow_sampler_;
    uint32_t shadow_map_size_ = 1;
    bool shadows_enabled_ = false;
    Float3 sun_direction_{};
    TextureRef white_srgb_, white_linear_, flat_normal_;  ///< neutral textures: base/emissive, metallic-roughness/occlusion, normal
    TextureRef env_brdf_;                                 ///< split-sum table (set 1); a single zero texel in Legacy shading
    SamplerRef default_sampler_, env_brdf_sampler_;
};

} // namespace saige::render
