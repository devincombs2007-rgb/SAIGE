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
// blending composites `alpha_blend` materials source-over in a sorted phase after all opaque draws (see MaterialData).
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
    /// glTF `BLEND`: the surface (base color alpha times the factor) is composited source-over with straight alpha in a sorted
    /// phase after all opaque and masked draws, testing but not writing depth. Only when the renderer was created with
    /// `ModelRenderSettings::alpha_blending` (off by default: the 0.2 behavior, BLEND drawn opaque) and a blendable scene format
    /// (see rhi/blend.hpp); otherwise the material is drawn opaque.
    bool alpha_blend = false;
    /// Whether this surface casts a sun shadow. It follows the *authored* alpha mode, not the pipeline the surface happens to be drawn
    /// with: opaque and MASK materials cast, BLEND materials do not (partial shadows are a later slice), including a BLEND that the
    /// renderer draws opaque because blending is off. A material with `alpha_cutoff > 0` casts through an alpha-tested shadow variant that
    /// reads the same bind group as the visible pass, so its texture, factor and cutoff are the visible pass's; with a cutoff of 0 (an opaque
    /// material, or a MASK that passes everything) it casts solid. Staging sets it from the glTF alpha mode; a hand-built material
    /// defaults to casting.
    bool casts_sun_shadow = true;
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
    uint32_t blended_primitives = 0; ///< primitives drawn in the sorted blended phase
    uint32_t blended_draws = 0;      ///< draw calls in that phase (two per double-sided primitive)
    uint32_t primitives_culled = 0;  ///< primitives skipped because their bounding sphere is outside the view frustum (see ModelRenderSettings::frustum_culling)
};

/// Renderer-local lighting selection. These values also identify the shader modes.
enum class ModelShading : uint8_t {
    Legacy = 0,             ///< SAIGE 0.2 lighting and base-color-only staging.
    Extended = 1,           ///< Legacy lighting with occlusion and emissive textures.
    MetallicRoughness = 2,  ///< glTF physically based shading.
};

/// The sun and ground ambient that light imported models (linear). The defaults are the fixed values every release before this
/// setting used, so leaving it alone changes nothing. The sky half of the ambient is not here: it is 0.6 times the sky color
/// (`ModelRenderSettings::sky`), which is also the fog color. The 0.1 cube and ground shaders keep their own fixed lighting.
struct SceneLighting {
    Float3 sun_direction{-0.40f, -0.55f, 0.73f};  ///< points toward the sun; not required to be unit length, must not be zero
    Float3 sun_color{1.00f, 0.95f, 0.85f};        ///< linear; may exceed 1 for an HDR output
    Float3 ambient_ground{0.20f, 0.18f, 0.16f};   ///< ambient light arriving from below
};

/// Throws std::invalid_argument if any component is not finite, a color is negative, or the sun direction has no length.
void validate_lighting(const SceneLighting& lighting);

/// Sun shadows for imported models (SceneRenderer drives them from RendererSettings::shadows). Off by default, and when off the renderer
/// uses the shadow-free shader and layout and acquires nothing for shadows: no shadow map, comparison sampler, pass or per-frame
/// buffers, and the device needs no sampled depth or comparison-sampler support.
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
    uint32_t filter_radius = 1;        ///< PCF radius in texels: (2r+1)^2 comparison taps
};

struct ModelRenderSettings {
    ModelShading shading = ModelShading::Legacy;
    SceneLighting lighting;
    ModelShadowSettings shadows;
    /// Composite glTF BLEND materials (`MaterialData::alpha_blend`) source-over in a sorted phase. **Off by default**: with it off a
    /// BLEND material draws opaque exactly as in 0.2, so saved 0.2 scenes look as they did until a host opts in.
    bool alpha_blending = false;
    rhi::ClearColor sky{0.30f, 0.50f, 0.82f, 1.0f};  ///< also the fog color (linear)
    float fog_density = 0.012f;                      ///< per meter
    /// Skip primitives whose bounding sphere (`GpuPrimitive::bounds_center`/`bounds_radius`, placed by the instance's world matrix, scaled by its
    /// largest axis scale) lies outside the frustum extracted from the `view_proj` given to `draw`. That matrix **must** be a reverse-Z infinite
    /// perspective projection times a view (what SceneRenderer builds), which is why this is off here and on in RendererSettings. The test is
    /// conservative (it never culls a visible sphere); a primitive without bounds (radius 0, a hand-built one) is never culled.
    bool frustum_culling = false;
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

    /// Whether the scene format can blend (the portable subset of rhi/blend.hpp). Fixed at construction.
    [[nodiscard]] bool blending_supported() const noexcept { return blending_supported_; }
    /// Whether the host asked for alpha blending (`ModelRenderSettings::alpha_blending`). Fixed at construction.
    [[nodiscard]] bool alpha_blending() const noexcept { return alpha_blending_; }
    /// Whether `alpha_blend` materials are really blended: asked for and supported. Otherwise they draw opaque, as in 0.2.
    [[nodiscard]] bool blending_enabled() const noexcept { return alpha_blending_ && blending_supported_; }

    /// The key blended primitives are sorted by, larger = farther = drawn first: the view-space depth of `world_center`, which for a
    /// perspective projection is the clip-space w. (Orthographic projections give every point the same w and would not sort; the
    /// renderer has perspective cameras only.)
    [[nodiscard]] static float blend_sort_depth(const Mat4& view_proj, Float3 world_center) noexcept;

    /// GPU textures the renderer itself keeps alive: three neutral slot textures and the environment BRDF table (visible in the
    /// registry's texture counters).
    static constexpr uint32_t kOwnedTextureCount = 4;  ///< +1 (the shadow map) when shadows are enabled

    /// The unit vector toward the sun: the one value the lighting constants are built from and the shadow fit reads, so the two cannot
    /// disagree (SceneLighting::sun_direction normalized).
    [[nodiscard]] Float3 sun_direction() const noexcept { return sun_direction_; }
    [[nodiscard]] bool shadows_enabled() const noexcept { return shadows_enabled_; }
    /// The sun shadow map: a D32_FLOAT texture, valid only when shadows are enabled. The caller (SceneRenderer's render graph) owns its
    /// synchronization: it must be written by the shadow pass and in `rhi::sampled_texture_state` while the scene pass draws.
    [[nodiscard]] rhi::TextureHandle shadow_map() const noexcept { return shadow_map_.handle(); }
    [[nodiscard]] uint32_t shadow_map_size() const noexcept { return shadow_map_size_; }
    /// Selects this frame's shadow block for draw(): call once per frame, between begin_frame and end_frame. `nullptr` means no shadows
    /// this frame (the sun is never occluded). The block lives in a ring of `DeviceInfo::frames_in_flight` buffers indexed by the device's
    /// frame number, so rewriting it never races the GPU's reads of an earlier frame. Frame-slot contract: one renderer, one write per
    /// frame. A second call in the same frame throws std::logic_error *before* anything is modified, because it would overwrite the block
    /// the first view's draws read. (Several views per frame need independent ModelRenderers.) Throws std::logic_error if shadows are off.
    void set_shadow_frame(const ModelShadowFrame* frame);
    /// Records the shadow pass's draws into an active depth-only rendering pass (the shadow map as its depth attachment, viewport and
    /// scissor set by the caller): the primitives of materials that cast (`MaterialData::casts_sun_shadow`), depth only; those with an alpha
    /// cutoff go through the alpha-tested variant with their material group bound. Throws std::logic_error if shadows are off.
    void draw_shadow(rhi::CommandList& cmd, const Mat4& light_view_proj, std::span<const ModelDraw> draws, ModelFrameStats& stats);

    [[nodiscard]] ModelShading shading() const noexcept { return shading_; }
    [[nodiscard]] rhi::BindGroupLayoutHandle material_layout() const noexcept { return material_layout_; }

private:
    [[nodiscard]] rhi::PipelineHandle pipeline_for(bool mirrored, bool double_sided) const noexcept;
    void ensure_blend_pipelines();
    void acquire(rhi::Format color_format, rhi::Format depth_format, const ModelRenderSettings& settings);
    void release() noexcept;  // everything the constructor may have acquired, in any state of completeness
    [[nodiscard]] rhi::BindGroupHandle constants_group() const noexcept;

    /// One primitive waiting for the sorted phase. Kept in a member vector that is cleared, not freed, each frame.
    struct BlendedDraw {
        float depth = 0.0f;
        uint32_t sequence = 0;  ///< submission order: the deterministic tie-break
        const GpuPrimitive* primitive = nullptr;
        const GpuMaterial* material = nullptr;
        Mat4 world;
        bool mirrored = false;
    };

    rhi::Device& device_;
    GpuResourceRegistry& registry_;
    ModelShading shading_;
    rhi::ShaderHandle vs_, ps_;
    rhi::BindGroupLayoutHandle material_layout_, constants_layout_;
    rhi::PipelineHandle pipelines_[4];  ///< [mirrored][double_sided]
    rhi::GraphicsPipelineDesc pipeline_template_;  ///< everything but cull, winding and blend, kept to create blend variants lazily
    rhi::PipelineHandle blend_pipelines_[4];       ///< [mirrored][pass]: pass 0 culls back faces (draws front faces), pass 1 culls front faces
    bool blend_pipelines_ready_ = false;
    bool blending_supported_ = false;
    bool alpha_blending_ = false;
    bool frustum_culling_ = false;
    std::vector<BlendedDraw> blended_;
    BindGroupRef constants_group_;  ///< set 1 with shadows off
    // Shadows on (the permutation with set 1 bindings 3..5); all empty or invalid when off.
    rhi::ShaderHandle shadow_vs_, shadow_mask_vs_, shadow_mask_ps_;
    rhi::PipelineHandle shadow_mask_pipeline_;  ///< alpha-tested variant, binds the material group at set 0
    rhi::PipelineHandle shadow_pipeline_;
    std::vector<BindGroupRef> constants_groups_;  ///< set 1 per frame slot: identical but for the shadow block
    std::vector<BufferRef> shadow_blocks_;
    uint32_t active_group_ = 0;
    TextureRef shadow_map_;
    SamplerRef shadow_sampler_;
    uint32_t shadow_map_size_ = 0;
    bool shadows_enabled_ = false;
    bool shadow_frame_written_ = false;
    uint64_t shadow_frame_number_ = 0;
    Float3 sun_direction_{};
    TextureRef white_srgb_, white_linear_, flat_normal_;  ///< neutral textures: base/emissive, metallic-roughness/occlusion, normal
    TextureRef env_brdf_;                                 ///< split-sum table (set 1); a single zero texel in Legacy shading
    SamplerRef default_sampler_, env_brdf_sampler_;
};

} // namespace saige::render
