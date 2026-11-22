// SAIGE Render — draws GpuModels inside a scene pass.
//
// Owned by SceneRenderer (created on first use) and recorded into its pass, so imported models share the depth buffer with
// the ground and the 0.1 cubes. Pipelines come in four variants (front face CCW or CW, culling back faces or none): the
// renderer picks the clockwise variant for an instance whose world matrix mirrors (negative determinant), so mirrored
// transforms are well-defined. Instances whose matrix is not finite, or whose determinant is ~zero, are skipped and counted.
//
// Materials in 0.2 sample the base color texture only; the rest of a glTF material's textures are not read. Alpha-mask
// materials discard below their cutoff; alpha blending is not implemented (a Blend material draws as opaque).
#pragma once

#include "saige/render/gpu_resources.hpp"
#include "saige/render/math.hpp"
#include "saige/render/model.hpp"
#include "saige/rhi/device.hpp"

#include <span>

namespace saige::render {

struct MaterialParams {
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  ///< linear factor
    float emissive[3] = {0.0f, 0.0f, 0.0f};          ///< linear, may exceed 1
    float alpha_cutoff = 0.0f;                       ///< > 0 discards fragments below it (alpha mask); 0 = opaque
    bool double_sided = false;
};

struct ModelFrameStats {
    uint32_t draw_calls = 0;
    uint32_t instances_drawn = 0;
    uint32_t instances_skipped = 0;  ///< non-finite or degenerate world matrix
};

struct ModelRenderSettings {
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

    /// Builds a material: a bind group with the texture (or white if empty), the sampler (or a default trilinear/repeat
    /// sampler if empty) and a uniform block of the parameters.
    [[nodiscard]] GpuMaterial create_material(const MaterialParams& params, const TextureRef& base_color, const SamplerRef& sampler,
                                              const char* name);

    /// Records the draws into an active rendering pass whose color and depth formats are the ones given at construction.
    void draw(rhi::CommandList& cmd, const Mat4& view_proj, Float3 camera_position, std::span<const ModelDraw> draws,
              ModelFrameStats& stats);

    [[nodiscard]] rhi::BindGroupLayoutHandle material_layout() const noexcept { return material_layout_; }

private:
    [[nodiscard]] rhi::PipelineHandle pipeline_for(bool mirrored, bool double_sided) const noexcept;

    rhi::Device& device_;
    GpuResourceRegistry& registry_;
    rhi::ShaderHandle vs_, ps_;
    rhi::BindGroupLayoutHandle material_layout_, constants_layout_;
    rhi::PipelineHandle pipelines_[4];  ///< [mirrored][double_sided]
    BindGroupRef constants_group_;
    TextureRef white_;
    SamplerRef default_sampler_;
};

} // namespace saige::render
