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
// ambient term, as glTF defines it) in both modes, and metallic-roughness in `ModelShading::MetallicRoughness`. The normal
// slot and its scale are bound and stored but not yet read by the shader (milestone M3). Alpha-mask materials discard below their cutoff; alpha
// blending is not implemented (a Blend material draws as opaque).
#pragma once

#include "saige/render/gpu_resources.hpp"
#include "saige/render/math.hpp"
#include "saige/render/model.hpp"
#include "saige/rhi/device.hpp"

#include <cstdint>
#include <span>

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
    float normal_scale = 1.0f;                       ///< (not yet shaded)
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

struct ModelRenderSettings {
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

    /// GPU textures the renderer itself keeps alive: three neutral slot textures and the environment BRDF table (visible in the
    /// registry's texture counters).
    static constexpr uint32_t kOwnedTextureCount = 4;

    [[nodiscard]] ModelShading shading() const noexcept { return shading_; }
    [[nodiscard]] rhi::BindGroupLayoutHandle material_layout() const noexcept { return material_layout_; }

private:
    [[nodiscard]] rhi::PipelineHandle pipeline_for(bool mirrored, bool double_sided) const noexcept;

    rhi::Device& device_;
    GpuResourceRegistry& registry_;
    ModelShading shading_;
    rhi::ShaderHandle vs_, ps_;
    rhi::BindGroupLayoutHandle material_layout_, constants_layout_;
    rhi::PipelineHandle pipelines_[4];  ///< [mirrored][double_sided]
    BindGroupRef constants_group_;
    TextureRef white_srgb_, white_linear_, flat_normal_;  ///< neutral textures: base/emissive, metallic-roughness/occlusion, normal
    TextureRef env_brdf_;                                 ///< split-sum table (set 1); a single zero texel in Legacy shading
    SamplerRef default_sampler_, env_brdf_sampler_;
};

} // namespace saige::render
