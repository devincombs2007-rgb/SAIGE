#include "saige/render/model_renderer.hpp"

#include "saige/render/camera.hpp"
#include "saige/render/env_brdf.hpp"
#include "saige/render/shadow_cascades.hpp"

#include "saige/shaders/scene_model.spv.hpp"
#include "saige/shaders/scene_model_ibl.spv.hpp"
#include "saige/shaders/scene_model_ibl_shadowed.spv.hpp"
#include "saige/shaders/scene_model_shadowed.spv.hpp"
#include "saige/shaders/scene_model_skinned.spv.hpp"
#include "saige/shaders/scene_shadow_depth.spv.hpp"
#include "saige/shaders/scene_shadow_depth_skinned.spv.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>

namespace saige::render {

namespace {

/// Push-constant block shared with shaders/scene/model.hlsl (keep in sync; layout checked below).
struct ModelPush {
    Mat4 view_proj;
    float model_row[3][4];  ///< the instance's 3x4 affine, one row per float4
    float camera_sign[4];   ///< camera position (xyz), determinant sign (w)
};
static_assert(sizeof(ModelPush) == 128, "ModelPush must fill exactly the guaranteed 128-byte push range");
static_assert(offsetof(ModelPush, model_row) == 64);
static_assert(offsetof(ModelPush, camera_sign) == 112);

/// Immutable per-renderer constants (set 1), matching `SceneConstants` in the shader.
struct SceneConstants {
    float fog_color_density[4];
    float sun_direction[4];
    float sun_color[4];
    float ambient_ground[4];
    float flags[4];  ///< x: shading mode, y: emissive gain
};

/// Shadow block (set 1, binding 5 of the shadowed permutation), matching `ShadowConstants` in the shader. Rewritten every frame.
struct ShadowBlock {
    float light_view_proj[4][16];  ///< world to each cascade's clip space, column-major like Mat4
    float texel[4];                ///< meters per shadow texel, cascades 0 to 3
    float splits[4];               ///< view-space depth where cascade 0 to 3 ends
    float params[4];               ///< normal offset (texels), filter radius (texels), cascade count, 1 when shadows are on
    float map[4];                  ///< 1 / tile size in texels, blend fraction, atlas tile scale x and y
    float camera_pos[4];           ///< the view the cascades are selected by
    float camera_fwd[4];           ///< w: atlas columns
};
static_assert(sizeof(ShadowBlock) == 352);

/// The parts of the block that depend only on the renderer: the tile size, the atlas layout, one cascade, shadows off.
void fill_shadow_layout(ShadowBlock& block, uint32_t tile_size, uint32_t columns, uint32_t rows) noexcept
{
    block.map[0] = 1.0f / static_cast<float>(tile_size);
    block.map[2] = 1.0f / static_cast<float>(columns);
    block.map[3] = 1.0f / static_cast<float>(rows);
    block.camera_fwd[3] = static_cast<float>(columns);
    block.params[2] = 1.0f;
}

/// Push constants of the shadow pass (shaders/scene/shadow_depth.hlsl).
struct ShadowPush {
    Mat4 light_view_proj;
    float model_row[3][4];
};
static_assert(sizeof(ShadowPush) == 112);

/// Bytes of one palette: kMaxSkinJoints affine matrices as three float4 rows (the shader's `skin_rows[768]`).
constexpr std::size_t kPaletteBytes = static_cast<std::size_t>(kMaxSkinJoints) * 48;
static_assert(kPaletteBytes <= rhi::max_uniform_binding_range);

/// IBL block (set 1, binding 8 of the IBL permutations), matching `IblConstants` in the shader.
struct IblBlock {
    float sh[9][4];  ///< radiance SH coefficients (rgb, padding)
    float params[4]; ///< levels - 1, intensity
};
static_assert(sizeof(IblBlock) == 160);

/// Material uniform block (set 0, binding 10), matching `Material` in the shader.
struct MaterialBlock {
    float base_color[4];
    float emissive_cutoff[4];
    float metallic_roughness_normal_occlusion[4];  ///< metallic, roughness, normal scale, occlusion strength
    float flags[4];                                ///< x: 1 when the material has a normal texture; y: 1 when it is blended
};
static_assert(sizeof(MaterialBlock) == 64);

/// Material bind group (set 0): textures at 0..4 in MaterialSlot order, their samplers at 5..9, the block at 10.
constexpr uint32_t kSamplerBase = kMaterialSlotCount;
constexpr uint32_t kBlockBinding = 2 * kMaterialSlotCount;

template <std::size_t N>
std::span<const uint32_t> words(const uint32_t (&array)[N])
{
    return {array, N};
}

bool finite_affine(const Mat4& m) noexcept
{
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 3; ++r) {
            if (!std::isfinite(m.at(r, c))) {
                return false;
            }
        }
    }
    return true;
}

float determinant3(const Mat4& m) noexcept
{
    const float a = m.at(0, 0), b = m.at(0, 1), c = m.at(0, 2);
    const float d = m.at(1, 0), e = m.at(1, 1), f = m.at(1, 2);
    const float g = m.at(2, 0), h = m.at(2, 1), i = m.at(2, 2);
    return a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
}

rhi::BufferHandle make_uniform(rhi::Device& device, GpuResourceRegistry& registry, BufferRef& out, const void* data,
                               std::size_t size, const char* name)
{
    rhi::BufferDesc ub;
    ub.size = std::max<std::size_t>(256, (size + 255) / 256 * 256);  // a multiple of every uniform alignment; only `size` bytes are bound
    ub.usage = rhi::BufferUsage::Uniform;
    ub.memory = rhi::MemoryLocation::CpuToGpu;
    ub.debug_name = name;
    out = registry.create_buffer(ub);
    std::span<std::byte> mapped = device.map(out.handle());
    if (mapped.size() < size) {
        throw std::runtime_error("ModelRenderer: uniform buffer is not host visible");
    }
    std::memcpy(mapped.data(), data, size);
    return out.handle();
}

} // namespace

uint64_t GpuModel::triangle_count() const noexcept
{
    uint64_t triangles = 0;
    for (const GpuPlacement& p : placements) {
        if (p.mesh < meshes.size()) {
            for (const GpuPrimitive& prim : meshes[p.mesh]) {
                triangles += prim.index_count / 3u;
            }
        }
    }
    return triangles;
}

namespace {

// Unit vector along `v` computed without squaring the components: dividing by the largest magnitude first keeps the sum of squares in
// [1, 3] for any finite v (a plain squared length overflows above about 1.8e19 and underflows below about 1e-19, which used to turn a
// valid direction into a zero or an infinity). Returns false for a zero vector.
bool unit_direction(Float3 v, Float3& out)
{
    const float m = std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
    if (!(m > 0.0f) || !std::isfinite(m)) return false;
    const Float3 s{v.x / m, v.y / m, v.z / m};
    const float len = std::sqrt(s.x * s.x + s.y * s.y + s.z * s.z);
    out = {s.x / len, s.y / len, s.z / len};
    return true;
}

} // namespace

bool unit_sun_direction(Float3 v, Float3& out) noexcept
{
    return unit_direction(v, out);
}

void validate_lighting(const SceneLighting& lighting)
{
    const auto finite = [](Float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    const auto non_negative = [](Float3 v) { return v.x >= 0.0f && v.y >= 0.0f && v.z >= 0.0f; };
    if (!finite(lighting.sun_direction) || !finite(lighting.sun_color) || !finite(lighting.ambient_ground) ||
        !non_negative(lighting.sun_color) || !non_negative(lighting.ambient_ground) || !std::isfinite(lighting.ambient_sky_floor) || lighting.ambient_sky_floor < 0.0f ||
        !std::isfinite(lighting.emissive_gain) || lighting.emissive_gain < 0.0f) {
        throw std::invalid_argument("SceneLighting must be finite, with non-negative colors");
    }
    Float3 unit;
    if (!unit_direction(lighting.sun_direction, unit)) throw std::invalid_argument("SceneLighting sun direction must not be zero");
}

ModelRenderer::ModelRenderer(rhi::Device& device, GpuResourceRegistry& registry, rhi::Format color_format,
                             rhi::Format depth_format, const ModelRenderSettings& settings)
    : device_(device), registry_(registry), shading_(settings.shading)
{
    validate_lighting(settings.lighting);  // before anything is acquired from the device
    if (settings.shadows.enabled) {
        const ModelShadowSettings& sh = settings.shadows;
        if (sh.map_size < 16 || sh.map_size > 8192 || !std::isfinite(sh.depth_bias_constant) || !std::isfinite(sh.depth_bias_slope)) {
            throw std::invalid_argument("ModelRenderer: shadow map size must be 16 to 8192 and the depth bias finite");
        }
        if (sh.cascades < 1 || sh.cascades > kMaxShadowCascades) {
            throw std::invalid_argument("ModelRenderer: the number of shadow cascades must be 1 to 4");
        }
    }
    shadows_enabled_ = settings.shadows.enabled;
    dynamic_lighting_ = settings.dynamic_lighting;
    if (settings.environment) {
        validate_environment_lighting(*settings.environment);
        if (settings.shading != ModelShading::MetallicRoughness) {
            throw std::invalid_argument("ModelRenderer: an environment needs metallic-roughness shading (the other modes would ignore it)");
        }
    }
    ibl_enabled_ = settings.environment != nullptr;
    skinning_enabled_ = settings.skinning;
    // A constructor that throws never runs the destructor, and the shader, layout and pipeline handles below are raw: so every failure
    // after the first acquisition releases whatever exists by this point (the RAII members release themselves), then rethrows. This is
    // also what makes a failed shadow acquisition (shader, pipeline, map, sampler, ring buffers, groups) leave nothing behind.
    try {
        acquire(color_format, depth_format, settings);
    } catch (...) {
        release();
        throw;
    }
}

void ModelRenderer::acquire(rhi::Format color_format, rhi::Format depth_format, const ModelRenderSettings& settings)
{
    using namespace saige::shaders;
    // The four permutations of model.hlsl: sun shadows and image-based lighting each switch on independently.
    std::span<const uint32_t> vs_words = words(scene_model_vs_main), ps_words = words(scene_model_ps_main);
    const char* vs_name = "SceneModel.VS";
    const char* ps_name = "SceneModel.PS";
    if (shadows_enabled_ && ibl_enabled_) {
        vs_words = words(scene_model_ibl_shadowed_vs_main);
        ps_words = words(scene_model_ibl_shadowed_ps_main);
        vs_name = "SceneModelIblShadowed.VS";
        ps_name = "SceneModelIblShadowed.PS";
    } else if (shadows_enabled_) {
        vs_words = words(scene_model_shadowed_vs_main);
        ps_words = words(scene_model_shadowed_ps_main);
        vs_name = "SceneModelShadowed.VS";
        ps_name = "SceneModelShadowed.PS";
    } else if (ibl_enabled_) {
        vs_words = words(scene_model_ibl_vs_main);
        ps_words = words(scene_model_ibl_ps_main);
        vs_name = "SceneModelIbl.VS";
        ps_name = "SceneModelIbl.PS";
    }
    vs_ = device_.create_shader({rhi::ShaderStage::Vertex, vs_words, "VSMain", vs_name});
    ps_ = device_.create_shader({rhi::ShaderStage::Pixel, ps_words, "PSMain", ps_name});

    rhi::BindGroupLayoutDesc material;
    for (uint32_t slot = 0; slot < kMaterialSlotCount; ++slot) {
        material.entries.push_back({slot, rhi::BindingType::SampledTexture});
        material.entries.push_back({kSamplerBase + slot, rhi::BindingType::Sampler});
    }
    material.entries.push_back({kBlockBinding, rhi::BindingType::UniformBuffer});
    material.debug_name = "ModelMaterialLayout";
    material_layout_ = device_.create_bind_group_layout(material);
    rhi::BindGroupLayoutDesc constants;
    constants.entries = {{0, rhi::BindingType::UniformBuffer}, {1, rhi::BindingType::SampledTexture}, {2, rhi::BindingType::Sampler}};
    if (shadows_enabled_) {  // the shadowed permutation: the sun shadow map, its comparison sampler and the per-frame shadow block
        constants.entries.push_back({3, rhi::BindingType::SampledTexture});
        constants.entries.push_back({4, rhi::BindingType::Sampler});
        constants.entries.push_back({5, rhi::BindingType::UniformBuffer});
    }
    if (ibl_enabled_) {  // the IBL permutations: the prefiltered environment, its sampler and the SH block
        constants.entries.push_back({6, rhi::BindingType::SampledTexture});
        constants.entries.push_back({7, rhi::BindingType::Sampler});
        constants.entries.push_back({8, rhi::BindingType::UniformBuffer});
    }
    constants.debug_name = "ModelConstantsLayout";
    constants_layout_ = device_.create_bind_group_layout(constants);

    rhi::GraphicsPipelineDesc pd;
    pd.vertex_shader = vs_;
    pd.pixel_shader = ps_;
    pd.vertex_layout.stride = sizeof(ModelVertex);
    pd.vertex_layout.attributes = {{0, rhi::Format::RGB32_FLOAT, offsetof(ModelVertex, position)},
                                   {1, rhi::Format::RGB32_FLOAT, offsetof(ModelVertex, normal)},
                                   {2, rhi::Format::RG32_FLOAT, offsetof(ModelVertex, uv)},
                                   {3, rhi::Format::RGBA32_FLOAT, offsetof(ModelVertex, tangent)}};
    pd.depth = {true, true, rhi::CompareOp::Greater};  // reverse-Z
    pd.color_formats = {color_format};
    pd.depth_format = depth_format;
    pd.push_constant_size = sizeof(ModelPush);
    pd.bind_group_layouts = {material_layout_, constants_layout_};
    pipeline_template_ = pd;
    blending_supported_ = rhi::is_blendable_color_format(color_format);
    alpha_blending_ = settings.alpha_blending;
    frustum_culling_ = settings.frustum_culling;
    static const char* const names[4] = {"SceneModel.CCW.Back", "SceneModel.CCW.None", "SceneModel.CW.Back", "SceneModel.CW.None"};
    for (int mirrored = 0; mirrored < 2; ++mirrored) {
        for (int two_sided = 0; two_sided < 2; ++two_sided) {
            pd.front_face = mirrored != 0 ? rhi::FrontFace::Clockwise : rhi::FrontFace::CounterClockwise;
            pd.cull_mode = two_sided != 0 ? rhi::CullMode::None : rhi::CullMode::Back;
            pd.debug_name = names[mirrored * 2 + two_sided];
            pipelines_[mirrored * 2 + two_sided] = device_.create_graphics_pipeline(pd);
        }
    }

    if (shadows_enabled_) {
        shadow_map_size_ = settings.shadows.map_size;
        shadow_cascades_ = settings.shadows.cascades;
        shadow_vs_ = device_.create_shader({rhi::ShaderStage::Vertex, words(scene_shadow_depth_vs_main), "VSMain", "SceneShadowDepth.VS"});
        rhi::GraphicsPipelineDesc sp;
        sp.vertex_shader = shadow_vs_;  // no pixel shader: depth only
        sp.vertex_layout = pd.vertex_layout;
        sp.vertex_layout.attributes.resize(1);  // positions only (location 0); the other attributes are not consumed by the depth shader
        sp.cull_mode = rhi::CullMode::None;  // both sides cast; the bias and the receiver's normal offset handle self-shadowing
        sp.depth = {true, true, rhi::CompareOp::Greater};
        sp.depth_format = rhi::Format::D32_FLOAT;
        sp.depth_bias = {settings.shadows.depth_bias_constant, settings.shadows.depth_bias_slope};
        sp.push_constant_size = sizeof(ShadowPush);
        sp.debug_name = "SceneShadowDepth";
        shadow_pipeline_ = device_.create_graphics_pipeline(sp);
    }

    if (skinning_enabled_) {
        // The skinned vertex stage (the pixel stage is the plain model one, whatever the permutation) and the joint palette at set 2.
        skin_vs_ = device_.create_shader({rhi::ShaderStage::Vertex, words(scene_model_skinned_vs_main), "VSMain", "SceneModelSkinned.VS"});
        rhi::BindGroupLayoutDesc palette;
        palette.entries = {{0, rhi::BindingType::UniformBuffer}};
        palette.debug_name = "ModelPaletteLayout";
        palette_layout_ = device_.create_bind_group_layout(palette);
        rhi::GraphicsPipelineDesc sd = pipeline_template_;
        sd.vertex_shader = skin_vs_;
        sd.vertex_layout.stride = sizeof(SkinnedModelVertex);
        sd.vertex_layout.attributes.push_back({4, rhi::Format::RGBA16_UINT, offsetof(SkinnedModelVertex, joints)});
        sd.vertex_layout.attributes.push_back({5, rhi::Format::RGBA32_FLOAT, offsetof(SkinnedModelVertex, weights)});
        sd.bind_group_layouts = {material_layout_, constants_layout_, palette_layout_};
        skin_pipeline_template_ = sd;
        static const char* const skin_names[4] = {"SceneModelSkinned.CCW.Back", "SceneModelSkinned.CCW.None", "SceneModelSkinned.CW.Back",
                                                  "SceneModelSkinned.CW.None"};
        for (int mirrored = 0; mirrored < 2; ++mirrored) {
            for (int two_sided = 0; two_sided < 2; ++two_sided) {
                sd.front_face = mirrored != 0 ? rhi::FrontFace::Clockwise : rhi::FrontFace::CounterClockwise;
                sd.cull_mode = two_sided != 0 ? rhi::CullMode::None : rhi::CullMode::Back;
                sd.debug_name = skin_names[mirrored * 2 + two_sided];
                skinned_pipelines_[mirrored * 2 + two_sided] = device_.create_graphics_pipeline(sd);
            }
        }
        if (shadows_enabled_) {
            skin_shadow_vs_ = device_.create_shader({rhi::ShaderStage::Vertex, words(scene_shadow_depth_skinned_vs_main), "VSMain", "SceneShadowDepthSkinned.VS"});
            rhi::GraphicsPipelineDesc sp;
            sp.vertex_shader = skin_shadow_vs_;
            sp.vertex_layout.stride = sizeof(SkinnedModelVertex);
            sp.vertex_layout.attributes = {{0, rhi::Format::RGB32_FLOAT, offsetof(ModelVertex, position)},
                                           {4, rhi::Format::RGBA16_UINT, offsetof(SkinnedModelVertex, joints)},
                                           {5, rhi::Format::RGBA32_FLOAT, offsetof(SkinnedModelVertex, weights)}};
            sp.cull_mode = rhi::CullMode::None;
            sp.depth = {true, true, rhi::CompareOp::Greater};
            sp.depth_format = rhi::Format::D32_FLOAT;
            sp.depth_bias = {settings.shadows.depth_bias_constant, settings.shadows.depth_bias_slope};
            sp.push_constant_size = sizeof(ShadowPush);
            sp.bind_group_layouts = {palette_layout_};  // the depth stage needs the palette only
            sp.debug_name = "SceneShadowDepthSkinned";
            skin_shadow_pipeline_ = device_.create_graphics_pipeline(sp);
        }
        identity_palette_ = make_palette(kMaxSkinJoints, "ModelIdentityPalette");  // the bind pose, for skinned primitives drawn without a palette
    }

    // Immutable scene constants. The default lighting is the same sun, ambient and fog as the 0.1 cube and ground shaders.
    const SceneLighting& light = settings.lighting;
    Float3 sun_unit{};
    if (!unit_direction(light.sun_direction, sun_unit)) throw std::invalid_argument("SceneLighting sun direction must not be zero");
    sun_direction_ = sun_unit;  // the one normalized direction: the lighting constants below and the shadow fit both use it
    SceneConstants sc{};
    sc.fog_color_density[0] = settings.sky.r;
    sc.fog_color_density[1] = settings.sky.g;
    sc.fog_color_density[2] = settings.sky.b;
    sc.fog_color_density[3] = settings.fog_density;
    sc.sun_direction[0] = sun_unit.x;
    sc.sun_direction[1] = sun_unit.y;
    sc.sun_direction[2] = sun_unit.z;
    sc.sun_color[0] = light.sun_color.x;
    sc.sun_color[1] = light.sun_color.y;
    sc.sun_color[2] = light.sun_color.z;
    sc.ambient_ground[0] = light.ambient_ground.x;
    sc.ambient_ground[1] = light.ambient_ground.y;
    sc.ambient_ground[2] = light.ambient_ground.z;
    sc.ambient_ground[3] = light.ambient_sky_floor;
    sc.flags[0] = static_cast<float>(shading_);
    sc.flags[1] = light.emissive_gain;
    static_assert(sizeof(SceneConstants) == 80);
    std::memcpy(scene_default_.data(), &sc, sizeof(sc));
    frame_sun_ = sun_unit;
    BufferRef constants_buffer;
    make_uniform(device_, registry_, constants_buffer, &sc, sizeof(sc), "ModelSceneConstants");
    const uint32_t light_slots = std::max(1u, device_.info().frames_in_flight);
    if (dynamic_lighting_) {  // one copy of the scene constants per frame slot, each rewritten by the frame that owns the slot
        scene_blocks_.reserve(light_slots);
        for (uint32_t i = 0; i < light_slots; ++i) {
            BufferRef block;
            make_uniform(device_, registry_, block, &sc, sizeof(sc), "ModelSceneConstantsFrame");
            scene_blocks_.push_back(std::move(block));
        }
    }
    // Neutral 1x1 textures, so a slot a material leaves empty changes nothing: white for base color, emissive (the factor
    // alone decides), metallic-roughness and occlusion (factors alone decide), and a flat tangent-space normal (0, 0, 1).
    const auto neutral = [&](rhi::Format format, const char* debug, uint8_t r, uint8_t g, uint8_t b) {
        rhi::TextureDesc d;
        d.format = format;
        d.width = d.height = 1;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        d.debug_name = debug;
        const std::byte texel[4] = {std::byte{r}, std::byte{g}, std::byte{b}, std::byte{255}};
        return registry_.create_texture(d, texel);
    };
    white_srgb_ = neutral(rhi::Format::RGBA8_SRGB, "ModelWhite", 255, 255, 255);
    white_linear_ = neutral(rhi::Format::RGBA8_UNORM, "ModelWhiteLinear", 255, 255, 255);
    flat_normal_ = neutral(rhi::Format::RGBA8_UNORM, "ModelFlatNormal", 128, 128, 255);

    rhi::SamplerDesc sampler;  // trilinear, repeat
    default_sampler_ = registry_.get_sampler(sampler);

    // The split-sum environment BRDF table (metallic-roughness shading's ambient specular): the embedded, versioned asset.
    // Legacy shading never reads it, so it gets one zero texel.
    {
        const bool needed = settings.shading == ModelShading::MetallicRoughness;
        const uint16_t zero[2] = {0, 0};  // Legacy shading never reads the table: one zero texel
        const EnvBrdfAsset& asset = precomputed_env_brdf();
        const uint32_t table_size = needed ? asset.size : 1u;
        const uint16_t* table_texels = needed ? asset.texels : zero;
        const std::size_t table_values = needed ? asset.texel_count : 2u;
        rhi::TextureDesc d;
        d.format = rhi::Format::RG16_FLOAT;
        d.width = d.height = table_size;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        d.debug_name = "ModelEnvBrdf";
        env_brdf_ = registry_.create_texture(d, std::as_bytes(std::span(table_texels, table_values)));
        rhi::SamplerDesc s;
        s.address_u = s.address_v = rhi::AddressMode::ClampToEdge;
        s.mipmap_mode = rhi::MipmapMode::Nearest;
        s.max_lod = 0.0f;
        env_brdf_sampler_ = registry_.get_sampler(s);
    }

    // Image-based lighting: the prefiltered environment (a 2D RGBA16F texture with one mip per roughness level), its sampler and the SH block.
    if (ibl_enabled_) {
        const EnvironmentLighting& env = *settings.environment;
        // The prefiltered chain is read with trilinear filtering: refuse a device that cannot, before creating anything for it.
        if (!device_.supports_filtered_sampling(rhi::Format::RGBA16_FLOAT)) {
            throw std::invalid_argument("ModelRenderer: the device cannot sample RGBA16_FLOAT with linear filtering, which the image-based lighting environment needs");
        }
        rhi::TextureDesc d;
        d.format = rhi::Format::RGBA16_FLOAT;
        d.width = env.width;
        d.height = env.height;
        d.mip_levels = env.levels;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        d.debug_name = "ModelIblEnvironment";
        ibl_texture_ = registry_.create_texture(d, std::as_bytes(std::span(env.rgba16f)));
        rhi::SamplerDesc s;  // trilinear: lod = roughness * (levels - 1) interpolates between the prefiltered levels; longitude wraps, latitude clamps
        s.address_u = rhi::AddressMode::Repeat;
        s.address_v = rhi::AddressMode::ClampToEdge;
        s.max_lod = static_cast<float>(env.levels - 1);
        ibl_sampler_ = registry_.get_sampler(s);
        IblBlock block{};
        for (std::size_t k = 0; k < 9; ++k) {
            block.sh[k][0] = env.sh[k].x;
            block.sh[k][1] = env.sh[k].y;
            block.sh[k][2] = env.sh[k].z;
        }
        block.params[0] = static_cast<float>(env.levels - 1);
        block.params[1] = env.intensity;
        make_uniform(device_, registry_, ibl_block_, &block, sizeof(block), "ModelIblBlock");
    }
    const auto constant_bindings_for = [&](const BufferRef& scene_buffer, const BufferRef* shadow_block) {
        std::vector<BindGroupBinding> bindings(3);
        bindings[0].binding = 0;
        bindings[0].buffer = scene_buffer;
        bindings[0].size = sizeof(sc);
        bindings[1].binding = 1;
        bindings[1].texture = env_brdf_;
        bindings[2].binding = 2;
        bindings[2].sampler = env_brdf_sampler_;
        if (shadow_block != nullptr) {
            BindGroupBinding b3, b4, b5;
            b3.binding = 3;
            b3.texture = shadow_map_;
            b4.binding = 4;
            b4.sampler = shadow_sampler_;
            b5.binding = 5;
            b5.buffer = *shadow_block;
            b5.size = sizeof(ShadowBlock);
            bindings.insert(bindings.end(), {b3, b4, b5});
        }
        if (ibl_enabled_) {
            BindGroupBinding b6, b7, b8;
            b6.binding = 6;
            b6.texture = ibl_texture_;
            b7.binding = 7;
            b7.sampler = ibl_sampler_;
            b8.binding = 8;
            b8.buffer = ibl_block_;
            b8.size = sizeof(IblBlock);
            bindings.insert(bindings.end(), {b6, b7, b8});
        }
        return bindings;
    };

    if (!shadows_enabled_) {
        if (dynamic_lighting_) {  // one group per frame slot, each over that slot's scene-constants buffer
            constants_groups_.reserve(light_slots);
            for (uint32_t i = 0; i < light_slots; ++i) {
                constants_groups_.push_back(registry_.create_bind_group(constants_layout_, constant_bindings_for(scene_blocks_[i], nullptr), "ModelSceneConstants"));
            }
        } else {
            constants_group_ = registry_.create_bind_group(constants_layout_, constant_bindings_for(constants_buffer, nullptr), "ModelSceneConstants");
        }
        return;
    }

    // Shadows on: the sun shadow map and its comparison sampler.
    {
        rhi::TextureDesc d;
        d.format = rhi::Format::D32_FLOAT;
        d.width = shadow_atlas_width();  // one tile per cascade
        d.height = shadow_atlas_height();
        d.usage = rhi::TextureUsage::DepthStencilAttachment | rhi::TextureUsage::Sampled;
        d.debug_name = "ModelSunShadowMap";
        shadow_map_ = registry_.create_texture(d);
        rhi::SamplerDesc s;
        s.min_filter = s.mag_filter = rhi::Filter::Nearest;  // linear filtering of depth formats is optional; the shader filters with taps
        s.mipmap_mode = rhi::MipmapMode::Nearest;
        s.address_u = s.address_v = rhi::AddressMode::ClampToEdge;
        s.max_lod = 0.0f;
        s.compare_enable = true;
        s.compare_op = rhi::CompareOp::GreaterOrEqual;  // reverse-Z: lit when the receiver is at least as close to the sun as the occluder
        shadow_sampler_ = registry_.get_sampler(s);
    }

    // Set 1 exists once per frame slot: the groups are identical except for the buffer that holds the frame's shadow block, which the
    // CPU rewrites every frame and so must not be shared with a frame the GPU may still be reading.
    const uint32_t slots = std::max(1u, device_.info().frames_in_flight);
    ShadowBlock off{};  // params.w = 0: the sun is never occluded
    fill_shadow_layout(off, shadow_map_size_, shadow_atlas_columns(), shadow_atlas_rows());
    constants_groups_.reserve(slots);
    shadow_blocks_.reserve(slots);
    for (uint32_t i = 0; i < slots; ++i) {
        BufferRef block;
        make_uniform(device_, registry_, block, &off, sizeof(off), "ModelShadowBlock");
        constants_groups_.push_back(registry_.create_bind_group(constants_layout_, constant_bindings_for(dynamic_lighting_ ? scene_blocks_[i] : constants_buffer, &block), "ModelSceneConstants"));
        shadow_blocks_.push_back(std::move(block));
    }
}

ModelRenderer::~ModelRenderer()
{
    release();
}

void ModelRenderer::release() noexcept
{
    constants_groups_.clear();
    shadow_blocks_.clear();
    scene_blocks_.clear();
    constants_group_.reset();
    ibl_block_.reset();
    ibl_sampler_.reset();
    ibl_texture_.reset();
    shadow_sampler_.reset();
    shadow_map_.reset();
    white_srgb_.reset();
    white_linear_.reset();
    flat_normal_.reset();
    env_brdf_.reset();
    env_brdf_sampler_.reset();
    default_sampler_.reset();
    for (rhi::PipelineHandle& p : pipelines_) {
        if (p.is_valid()) device_.destroy(p);
        p = {};
    }
    if (blend_pipelines_ready_) {
        for (const rhi::PipelineHandle p : blend_pipelines_) {
            device_.destroy(p);
        }
        if (skinning_enabled_) {
            for (rhi::PipelineHandle& p : skinned_blend_pipelines_) {
                if (p.is_valid()) device_.destroy(p);
                p = {};
            }
        }
        blend_pipelines_ready_ = false;
    }
    identity_palette_ = SkinPalette{};  // before the layout it was created against
    for (rhi::PipelineHandle& p : skinned_pipelines_) {
        if (p.is_valid()) device_.destroy(p);
        p = {};
    }
    if (skin_shadow_pipeline_.is_valid()) device_.destroy(skin_shadow_pipeline_);
    skin_shadow_pipeline_ = {};
    if (skin_shadow_vs_.is_valid()) device_.destroy(skin_shadow_vs_);
    skin_shadow_vs_ = {};
    if (skin_vs_.is_valid()) device_.destroy(skin_vs_);
    skin_vs_ = {};
    if (palette_layout_.is_valid()) device_.destroy(palette_layout_);
    palette_layout_ = {};
    if (shadow_pipeline_.is_valid()) device_.destroy(shadow_pipeline_);
    shadow_pipeline_ = {};
    if (shadow_vs_.is_valid()) device_.destroy(shadow_vs_);
    shadow_vs_ = {};
    if (ps_.is_valid()) device_.destroy(ps_);
    ps_ = {};
    if (vs_.is_valid()) device_.destroy(vs_);
    vs_ = {};
    if (material_layout_.is_valid()) device_.destroy(material_layout_);
    material_layout_ = {};
    if (constants_layout_.is_valid()) device_.destroy(constants_layout_);
    constants_layout_ = {};
}

rhi::BindGroupHandle ModelRenderer::constants_group() const noexcept
{
    return (shadows_enabled_ || dynamic_lighting_) ? constants_groups_[active_group_].handle() : constants_group_.handle();
}

GpuMaterial ModelRenderer::create_material(const MaterialData& data, const TextureRef& base_color, const SamplerRef& sampler,
                                           const char* name)
{
    MaterialTextures textures;
    textures[MaterialSlot::BaseColor] = {base_color, sampler};
    return create_material(data, textures, name);
}

GpuMaterial ModelRenderer::create_material(const MaterialData& data, const MaterialTextures& textures, const char* name)
{
    MaterialBlock block{};
    std::memcpy(block.base_color, data.base_color, sizeof(block.base_color));
    block.emissive_cutoff[0] = data.emissive[0];
    block.emissive_cutoff[1] = data.emissive[1];
    block.emissive_cutoff[2] = data.emissive[2];
    block.emissive_cutoff[3] = data.alpha_cutoff;
    block.metallic_roughness_normal_occlusion[0] = data.metallic;
    block.metallic_roughness_normal_occlusion[1] = data.roughness;
    block.metallic_roughness_normal_occlusion[2] = data.normal_scale;
    block.metallic_roughness_normal_occlusion[3] = data.occlusion_strength;
    block.flags[0] = textures[MaterialSlot::Normal].texture.valid() ? 1.0f : 0.0f;
    const bool blend = data.alpha_blend && blending_enabled();
    block.flags[1] = blend ? 1.0f : 0.0f;
    if (blend) {
        ensure_blend_pipelines();  // outside any rendering, so a device failure surfaces as a failed material, not a failed frame
    }
    BufferRef uniform;
    make_uniform(device_, registry_, uniform, &block, sizeof(block), name);

    const TextureRef* neutral[kMaterialSlotCount] = {&white_srgb_, &white_linear_, &flat_normal_, &white_linear_, &white_srgb_};
    BindGroupBinding bindings[2 * kMaterialSlotCount + 1];
    for (uint32_t slot = 0; slot < kMaterialSlotCount; ++slot) {
        const MaterialTextureSlot& s = textures.slots[slot];
        bindings[slot].binding = slot;
        bindings[slot].texture = s.texture.valid() ? s.texture : *neutral[slot];
        bindings[kSamplerBase + slot].binding = kSamplerBase + slot;
        bindings[kSamplerBase + slot].sampler = s.sampler.valid() ? s.sampler : default_sampler_;
    }
    bindings[kBlockBinding].binding = kBlockBinding;
    bindings[kBlockBinding].buffer = uniform;
    bindings[kBlockBinding].size = sizeof(block);
    GpuMaterial material;
    material.group = registry_.create_bind_group(material_layout_, bindings, name);
    material.double_sided = data.double_sided;
    material.blend = blend;
    material.casts_sun_shadow = data.casts_sun_shadow;
    return material;
}

void ModelRenderer::ensure_blend_pipelines()
{
    if (blend_pipelines_ready_) {
        return;
    }
    rhi::GraphicsPipelineDesc pd = pipeline_template_;
    pd.depth = {true, false, rhi::CompareOp::Greater};  // test against the opaque depth, never write it
    pd.blend = {rhi::BlendState::source_over()};        // straight alpha, as glTF defines BLEND
    static const char* const names[4] = {"SceneModel.Blend.CCW.Front", "SceneModel.Blend.CCW.Back", "SceneModel.Blend.CW.Front",
                                         "SceneModel.Blend.CW.Back"};
    static const char* const skin_names[4] = {"SceneModelSkinned.Blend.CCW.Front", "SceneModelSkinned.Blend.CCW.Back",
                                              "SceneModelSkinned.Blend.CW.Front", "SceneModelSkinned.Blend.CW.Back"};
    rhi::PipelineHandle created[8];
    int count = 0;
    try {
        for (int mirrored = 0; mirrored < 2; ++mirrored) {
            for (int pass = 0; pass < 2; ++pass) {
                pd.front_face = mirrored != 0 ? rhi::FrontFace::Clockwise : rhi::FrontFace::CounterClockwise;
                pd.cull_mode = pass == 0 ? rhi::CullMode::Back : rhi::CullMode::Front;
                pd.debug_name = names[mirrored * 2 + pass];
                created[count] = device_.create_graphics_pipeline(pd);
                ++count;
            }
        }
        if (skinning_enabled_) {
            rhi::GraphicsPipelineDesc sd = skin_pipeline_template_;
            sd.depth = pd.depth;
            sd.blend = pd.blend;
            for (int mirrored = 0; mirrored < 2; ++mirrored) {
                for (int pass = 0; pass < 2; ++pass) {
                    sd.front_face = mirrored != 0 ? rhi::FrontFace::Clockwise : rhi::FrontFace::CounterClockwise;
                    sd.cull_mode = pass == 0 ? rhi::CullMode::Back : rhi::CullMode::Front;
                    sd.debug_name = skin_names[mirrored * 2 + pass];
                    created[count] = device_.create_graphics_pipeline(sd);
                    ++count;
                }
            }
        }
    } catch (...) {
        for (int i = 0; i < count; ++i) {
            device_.destroy(created[i]);
        }
        throw;
    }
    for (int i = 0; i < 4; ++i) {
        blend_pipelines_[i] = created[i];
        if (skinning_enabled_) skinned_blend_pipelines_[i] = created[4 + i];
    }
    blend_pipelines_ready_ = true;
}

float ModelRenderer::blend_sort_depth(const Mat4& view_proj, Float3 c) noexcept
{
    // Row 3 of the column-major matrix: clip w = dot(row3, (c, 1)), the distance along the view direction for a perspective projection.
    const float w = view_proj.at(3, 0) * c.x + view_proj.at(3, 1) * c.y + view_proj.at(3, 2) * c.z + view_proj.at(3, 3);
    return std::isfinite(w) ? w : 0.0f;  // a non-finite key would break the sort's ordering: such a primitive sorts as if at the camera
}

bool ModelRenderer::posed_world_bounds(const GpuPrimitive& prim, const SkinPalette& palette, const Mat4& world, Float3& lo, Float3& hi) noexcept
{
    constexpr float inf = std::numeric_limits<float>::infinity();
    lo = {inf, inf, inf};
    hi = {-inf, -inf, -inf};
    const std::size_t joints = prim.joint_bounds.size();
    if (joints > palette.joint_count_ || joints * 12 > palette.rows_.size()) return false;  // the palette must hold a matrix for every joint the bounds name
    for (std::size_t j = 0; j < joints; ++j) {
        const JointBounds& jb = prim.joint_bounds[j];
        if (!jb.used) continue;
        const float* s = &palette.rows_[j * 12];  // the joint's skin matrix: three rows of (x, y, z, translation)
        // world * skin: both affine, so the product's rows are world's 3x3 times skin's rows, plus world's translation in the last column.
        float m[3][4];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                m[r][c] = world.at(r, 0) * s[c] + world.at(r, 1) * s[4 + c] + world.at(r, 2) * s[8 + c] + (c == 3 ? world.at(r, 3) : 0.0f);
            }
        }
        const float cx = (jb.min.x + jb.max.x) * 0.5f, cy = (jb.min.y + jb.max.y) * 0.5f, cz = (jb.min.z + jb.max.z) * 0.5f;
        const float hx = (jb.max.x - jb.min.x) * 0.5f, hy = (jb.max.y - jb.min.y) * 0.5f, hz = (jb.max.z - jb.min.z) * 0.5f;
        for (int r = 0; r < 3; ++r) {
            const float center = m[r][0] * cx + m[r][1] * cy + m[r][2] * cz + m[r][3];
            const float extent = std::fabs(m[r][0]) * hx + std::fabs(m[r][1]) * hy + std::fabs(m[r][2]) * hz;
            if (!std::isfinite(center) || !std::isfinite(extent)) return false;
            float* l = r == 0 ? &lo.x : (r == 1 ? &lo.y : &lo.z);
            float* h = r == 0 ? &hi.x : (r == 1 ? &hi.y : &hi.z);
            *l = std::min(*l, center - extent);
            *h = std::max(*h, center + extent);
        }
    }
    if (!(lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z)) return false;  // no joint is used
    // The weights of a vertex sum to S within 1e-3 of one (the tolerance staging accepts; the importer normalizes to float precision). With world = L x + t, a posed vertex is
    // L (S c) + t = (L c + t) + (S - 1) L c for a point c of the hull in pose space, so it can leave the box by |S - 1| |L c| <= 1e-3 (|L c + t| + |t|): the box's distance from the
    // origin plus the placement's translation, times 1e-3, plus a hair for float rounding.
    const float reach = std::max({std::fabs(lo.x), std::fabs(lo.y), std::fabs(lo.z), std::fabs(hi.x), std::fabs(hi.y), std::fabs(hi.z)});
    const float shift = std::max({std::fabs(world.at(0, 3)), std::fabs(world.at(1, 3)), std::fabs(world.at(2, 3))});
    const float margin = 1e-3f * (reach + shift) + 1e-4f;
    lo = {lo.x - margin, lo.y - margin, lo.z - margin};
    hi = {hi.x + margin, hi.y + margin, hi.z + margin};
    return true;
}

SkinPalette ModelRenderer::make_palette(uint32_t joint_count, const char* name)
{
    const uint32_t slots = std::max(1u, device_.info().frames_in_flight);
    SkinPalette palette;
    palette.rows_.assign(static_cast<std::size_t>(kMaxSkinJoints) * 12, 0.0f);
    for (uint32_t j = 0; j < joint_count; ++j) {  // identity: the bind pose
        palette.rows_[static_cast<std::size_t>(j) * 12 + 0] = 1.0f;
        palette.rows_[static_cast<std::size_t>(j) * 12 + 5] = 1.0f;
        palette.rows_[static_cast<std::size_t>(j) * 12 + 10] = 1.0f;
    }
    palette.generation_ = 1;
    palette.slot_generation_.assign(slots, 0);  // every slot is stale until its first draw uploads the pose
    palette.slot_frame_.assign(slots, ~uint64_t{0});  // and has not been bound by any frame
    palette.buffers_.reserve(slots);
    palette.groups_.reserve(slots);
    for (uint32_t i = 0; i < slots; ++i) {
        rhi::BufferDesc ub;
        ub.size = kPaletteBytes;
        ub.usage = rhi::BufferUsage::Uniform;
        ub.memory = rhi::MemoryLocation::CpuToGpu;
        ub.debug_name = name;
        BufferRef buffer = registry_.create_buffer(ub);
        if (device_.map(buffer.handle()).size() < kPaletteBytes) {
            throw std::runtime_error("ModelRenderer: the skin palette buffer is not host visible");
        }
        BindGroupBinding binding;
        binding.binding = 0;
        binding.buffer = buffer;
        binding.size = kPaletteBytes;
        palette.groups_.push_back(registry_.create_bind_group(palette_layout_, std::span<const BindGroupBinding>(&binding, 1), name));
        palette.buffers_.push_back(std::move(buffer));
    }
    palette.joint_count_ = joint_count;
    palette.device_ = &device_;
    return palette;
}

SkinPalette ModelRenderer::create_skin_palette(uint32_t joint_count, const char* name)
{
    if (!skinning_enabled_) {
        throw std::logic_error("ModelRenderer::create_skin_palette: skinning is not enabled (ModelRenderSettings::skinning)");
    }
    if (joint_count == 0 || joint_count > kMaxSkinJoints) {
        throw std::invalid_argument("ModelRenderer::create_skin_palette: the joint count must be 1 to kMaxSkinJoints");
    }
    return make_palette(joint_count, name);
}

void ModelRenderer::check_palettes_unfrozen(std::span<const ModelDraw> draws) const
{
    const uint64_t frame = device_.frame_number();
    for (const ModelDraw& d : draws) {
        const SkinPalette* p = d.palette;
        if (p == nullptr || !p->valid() || p->groups_.empty()) continue;
        const std::size_t slot = static_cast<std::size_t>(frame % p->groups_.size());
        if (p->slot_frame_[slot] == frame && p->slot_generation_[slot] != p->generation_) {
            throw std::logic_error("ModelRenderer: a skin palette was updated after its first use in this frame (its pose is frozen for the frame; update palettes before the first draw)");
        }
    }
}

rhi::BindGroupHandle ModelRenderer::palette_group(const SkinPalette& palette)
{
    const std::size_t slot = static_cast<std::size_t>(device_.frame_number() % palette.groups_.size());
    palette.slot_frame_[slot] = device_.frame_number();  // from here on the slot's pose is what this frame's draws reference
    if (palette.slot_generation_[slot] != palette.generation_) {
        // The slot's buffer was last read by the frame that used this slot, which the device has finished before begin_frame returned.
        std::span<std::byte> mapped = device_.map(palette.buffers_[slot].handle());
        std::memcpy(mapped.data(), palette.rows_.data(), kPaletteBytes);
        palette.slot_generation_[slot] = palette.generation_;
    }
    return palette.groups_[slot].handle();
}

rhi::PipelineHandle ModelRenderer::pipeline_for(bool mirrored, bool double_sided) const noexcept
{
    return pipelines_[(mirrored ? 2 : 0) + (double_sided ? 1 : 0)];
}

void ModelRenderer::draw(rhi::CommandList& cmd, const Mat4& view_proj, Float3 camera_position, std::span<const ModelDraw> draws,
                         ModelFrameStats& stats)
{
    if (dynamic_lighting_) {
        // Complete the frame's slot before anything reads it: lighting that was not published is the construction-time lighting, a shadow
        // frame that was not set is "no shadows", and the draw reads this frame's slot rather than whichever slot was written last.
        const uint64_t frame_number = device_.frame_number();
        if (!(lighting_written_ && lighting_frame_number_ == frame_number)) write_scene_block(scene_default_.data(), sun_direction_);
        if (shadows_enabled_ && !(shadow_frame_written_ && shadow_frame_number_ == frame_number)) set_shadow_frame(nullptr);
        active_group_ = static_cast<uint32_t>(frame_number % constants_groups_.size());
    }
    check_palettes_unfrozen(draws);  // before anything is recorded or written
    rhi::PipelineHandle bound{};
    rhi::BindGroupHandle bound_palette{};  // set 2 of the bound skinned pipeline
    blended_.clear();
    uint32_t sequence = 0;
    std::optional<Frustum> frustum;
    if (frustum_culling_) frustum = extract_frustum(view_proj);
    for (const ModelDraw& d : draws) {
        if (d.model == nullptr) {
            continue;
        }
        for (const GpuPlacement& placement : d.model->placements) {
            if (placement.mesh >= d.model->meshes.size()) {
                continue;
            }
            const Mat4 world = d.world * placement.local;
            const float det = determinant3(world);
            if (!finite_affine(world) || !std::isfinite(det) || std::fabs(det) < 1e-12f) {
                ++stats.instances_skipped;  // a non-finite or degenerate transform never reaches the GPU
                continue;
            }
            ModelPush push{};
            push.view_proj = view_proj;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 4; ++c) {
                    push.model_row[r][c] = world.at(r, c);
                }
            }
            push.camera_sign[0] = camera_position.x;
            push.camera_sign[1] = camera_position.y;
            push.camera_sign[2] = camera_position.z;
            push.camera_sign[3] = det < 0.0f ? -1.0f : 1.0f;
            const bool mirrored = det < 0.0f;
            // The largest axis scale of the instance: a sphere stays a sphere bounded by it under any affine map.
            float max_scale = 0.0f;
            if (frustum) {
                for (int col = 0; col < 3; ++col) {
                    const float sx = world.at(0, col), sy = world.at(1, col), sz = world.at(2, col);
                    max_scale = std::max(max_scale, std::sqrt(sx * sx + sy * sy + sz * sz));
                }
            }

            for (const GpuPrimitive& prim : d.model->meshes[placement.mesh]) {
                if (frustum && prim.skinned) {
                    // A skinned primitive's own bounds are its bind pose's, which say nothing about the posed mesh. With per-joint bounds the pose gives a conservative world box:
                    // the union of each used joint's bind-space box carried by that joint's skin matrix and the instance's world matrix. Without them it is never culled.
                    const SkinPalette* pose = d.palette != nullptr ? d.palette : &identity_palette_;
                    bool culled = false;
                    if (!prim.joint_bounds.empty() && pose->valid() && pose->joint_count() >= prim.joint_limit) {
                        Float3 lo, hi;
                        if (posed_world_bounds(prim, *pose, world, lo, hi)) {
                            culled = !intersects_aabb(*frustum, lo, hi);
                        } else {
                            ++stats.skinned_cull_bypassed;
                        }
                    } else {
                        ++stats.skinned_cull_bypassed;
                    }
                    if (culled) {
                        ++stats.primitives_culled;
                        ++stats.skinned_culled;
                        continue;
                    }
                } else if (frustum && prim.bounds_radius > 0.0f) {
                    const Float3 local = prim.bounds_center;
                    const Float3 center{world.at(0, 0) * local.x + world.at(0, 1) * local.y + world.at(0, 2) * local.z + world.at(0, 3),
                                        world.at(1, 0) * local.x + world.at(1, 1) * local.y + world.at(1, 2) * local.z + world.at(1, 3),
                                        world.at(2, 0) * local.x + world.at(2, 1) * local.y + world.at(2, 2) * local.z + world.at(2, 3)};
                    if (!intersects_sphere(*frustum, center, prim.bounds_radius * max_scale)) {
                        ++stats.primitives_culled;
                        continue;
                    }
                }
                const GpuMaterial& material = d.model->materials[prim.material];
                const SkinPalette* palette = nullptr;
                if (prim.skinned) {
                    palette = d.palette != nullptr ? d.palette : &identity_palette_;
                    if (!skinning_enabled_ || !palette->valid() || palette->joint_count() < prim.joint_limit) {
                        ++stats.skinned_skipped;  // never read past the palette, never feed skinned vertices to a rigid pipeline
                        continue;
                    }
                }
                if (material.blend && blend_pipelines_ready_) {
                    // Deferred: blended primitives draw after every opaque one, sorted back to front (below).
                    const Float3 local = prim.bounds_center;
                    const Float3 center{world.at(0, 0) * local.x + world.at(0, 1) * local.y + world.at(0, 2) * local.z + world.at(0, 3),
                                        world.at(1, 0) * local.x + world.at(1, 1) * local.y + world.at(1, 2) * local.z + world.at(1, 3),
                                        world.at(2, 0) * local.x + world.at(2, 1) * local.y + world.at(2, 2) * local.z + world.at(2, 3)};
                    blended_.push_back({blend_sort_depth(view_proj, center), sequence++, &prim, &material, world, mirrored, palette});
                    continue;
                }
                const rhi::PipelineHandle pipeline = palette != nullptr ? skinned_pipelines_[(mirrored ? 2 : 0) + (material.double_sided ? 1 : 0)]
                                                                         : pipeline_for(mirrored, material.double_sided);
                if (pipeline != bound) {
                    cmd.bind_pipeline(pipeline);
                    cmd.bind_group(1, constants_group());  // a new pipeline starts with nothing bound
                    bound = pipeline;
                    bound_palette = {};
                }
                if (palette != nullptr) {
                    const rhi::BindGroupHandle group = palette_group(*palette);
                    if (group != bound_palette) {
                        cmd.bind_group(2, group);
                        bound_palette = group;
                    }
                    ++stats.skinned_draws;
                }
                cmd.bind_group(0, material.group.handle());
                cmd.bind_vertex_buffer(prim.vertices.handle());
                cmd.bind_index_buffer(prim.indices.handle(), prim.index_type);
                cmd.push_constant_struct(push);
                cmd.draw_indexed(prim.index_count);
                ++stats.draw_calls;
            }
            ++stats.instances_drawn;
        }
    }

    // Sorted phase: farthest first by the view-space depth of each primitive's bounding-sphere center, submission order breaking
    // ties, so a frame is reproducible. std::sort with a total order does not allocate; blended_ keeps its capacity.
    std::sort(blended_.begin(), blended_.end(), [](const BlendedDraw& a, const BlendedDraw& b) {
        return a.depth != b.depth ? a.depth > b.depth : a.sequence < b.sequence;
    });
    for (const BlendedDraw& item : blended_) {
        ModelPush push{};
        push.view_proj = view_proj;
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) {
                push.model_row[r][c] = item.world.at(r, c);
            }
        }
        push.camera_sign[0] = camera_position.x;
        push.camera_sign[1] = camera_position.y;
        push.camera_sign[2] = camera_position.z;
        push.camera_sign[3] = item.mirrored ? -1.0f : 1.0f;
        // A double-sided surface draws its back faces first, then its front faces, so a convex mesh composites correctly
        // against itself; a single-sided one draws its front faces only.
        const int first_pass = item.material->double_sided ? 1 : 0;
        for (int pass = first_pass; pass >= 0; --pass) {
            const rhi::PipelineHandle pipeline = item.palette != nullptr ? skinned_blend_pipelines_[(item.mirrored ? 2 : 0) + pass]
                                                                         : blend_pipelines_[(item.mirrored ? 2 : 0) + pass];
            if (pipeline != bound) {
                cmd.bind_pipeline(pipeline);
                cmd.bind_group(1, constants_group());
                bound = pipeline;
                bound_palette = {};
            }
            if (item.palette != nullptr) {
                const rhi::BindGroupHandle group = palette_group(*item.palette);
                if (group != bound_palette) {
                    cmd.bind_group(2, group);
                    bound_palette = group;
                }
                ++stats.skinned_draws;
            }
            cmd.bind_group(0, item.material->group.handle());
            cmd.bind_vertex_buffer(item.primitive->vertices.handle());
            cmd.bind_index_buffer(item.primitive->indices.handle(), item.primitive->index_type);
            cmd.push_constant_struct(push);
            cmd.draw_indexed(item.primitive->index_count);
            ++stats.draw_calls;
            ++stats.blended_draws;
        }
        ++stats.blended_primitives;
    }
}

void ModelRenderer::write_scene_block(const float* constants, Float3 sun)
{
    const uint64_t frame_number = device_.frame_number();
    const std::size_t slot = static_cast<std::size_t>(frame_number % scene_blocks_.size());
    std::span<std::byte> mapped = device_.map(scene_blocks_[slot].handle());  // may throw: nothing has been recorded as written
    std::memcpy(mapped.data(), constants, sizeof(SceneConstants));
    lighting_frame_number_ = frame_number;
    lighting_written_ = true;
    frame_sun_ = sun;
}

bool ModelRenderer::lighting_written_this_frame() const noexcept
{
    return dynamic_lighting_ && lighting_written_ && lighting_frame_number_ == device_.frame_number();
}

Float3 ModelRenderer::frame_sun_direction() const noexcept
{
    return dynamic_lighting_ && lighting_written_ && lighting_frame_number_ == device_.frame_number() ? frame_sun_ : sun_direction_;
}

void ModelRenderer::set_lighting_frame(const ModelLightingFrame* frame)
{
    if (!dynamic_lighting_) {
        throw std::logic_error("ModelRenderer::set_lighting_frame: dynamic lighting is not enabled (ModelRenderSettings::dynamic_lighting)");
    }
    const uint64_t frame_number = device_.frame_number();
    // The frame-slot contract: one write of the slot per renderer per frame, checked before anything changes. A draw completes an unpublished slot with the
    // construction-time lighting, so it counts as the frame's write: publishing after the first draw is refused by the same check.
    if (lighting_written_ && lighting_frame_number_ == frame_number) {
        throw std::logic_error("ModelRenderer::set_lighting_frame: this frame's slot is already written (one publication per renderer per frame, before its first draw)");
    }
    if (frame == nullptr) {
        write_scene_block(scene_default_.data(), sun_direction_);
        return;
    }
    validate_lighting(frame->lighting);
    const Float3 sky = frame->sky_color;
    if (!std::isfinite(sky.x) || !std::isfinite(sky.y) || !std::isfinite(sky.z) || sky.x < 0.0f || sky.y < 0.0f || sky.z < 0.0f) {
        throw std::invalid_argument("ModelLightingFrame: the sky color must be finite and not negative");
    }
    Float3 sun{};
    if (!unit_direction(frame->lighting.sun_direction, sun)) throw std::invalid_argument("SceneLighting sun direction must not be zero");
    SceneConstants sc{};
    std::memcpy(&sc, scene_default_.data(), sizeof(sc));  // keeps the fog density and the shading mode
    sc.fog_color_density[0] = sky.x;
    sc.fog_color_density[1] = sky.y;
    sc.fog_color_density[2] = sky.z;
    sc.sun_direction[0] = sun.x;
    sc.sun_direction[1] = sun.y;
    sc.sun_direction[2] = sun.z;
    sc.sun_color[0] = frame->lighting.sun_color.x;
    sc.sun_color[1] = frame->lighting.sun_color.y;
    sc.sun_color[2] = frame->lighting.sun_color.z;
    sc.ambient_ground[0] = frame->lighting.ambient_ground.x;
    sc.ambient_ground[1] = frame->lighting.ambient_ground.y;
    sc.ambient_ground[2] = frame->lighting.ambient_ground.z;
    sc.ambient_ground[3] = frame->lighting.ambient_sky_floor;
    sc.flags[1] = frame->lighting.emissive_gain;
    write_scene_block(reinterpret_cast<const float*>(&sc), sun);
}

void ModelRenderer::set_shadow_frame(const ModelShadowFrame* frame)
{
    if (!shadows_enabled_) {
        throw std::logic_error("ModelRenderer::set_shadow_frame: shadows are not enabled");
    }
    // The frame-slot contract: one write per renderer per frame, checked before anything is modified. A second write in the same frame
    // would replace the block the first view's draws are going to read.
    const uint64_t frame_number = device_.frame_number();
    if (shadow_frame_written_ && shadow_frame_number_ == frame_number) {
        throw std::logic_error("ModelRenderer::set_shadow_frame: already set for this frame (one write per renderer per frame)");
    }
    const uint32_t slot = static_cast<uint32_t>(frame_number % constants_groups_.size());
    if (frame != nullptr) {  // checked before anything is modified
        const bool bad = frame->cascade_count < 1 || frame->cascade_count > shadow_cascades_ || !std::isfinite(frame->blend) || frame->blend < 0.0f || frame->blend > 0.5f;
        if (bad) throw std::invalid_argument("ModelRenderer::set_shadow_frame: the cascade count is outside 1 to the renderer's cascades, or the blend is outside 0 to 0.5");
    }
    ShadowBlock block{};
    fill_shadow_layout(block, shadow_map_size_, shadow_atlas_columns(), shadow_atlas_rows());
    if (frame != nullptr) {
        std::memcpy(block.light_view_proj[0], &frame->light_view_proj, sizeof(block.light_view_proj[0]));
        block.texel[0] = frame->texel_world_size;
        for (uint32_t i = 1; i < frame->cascade_count; ++i) {
            std::memcpy(block.light_view_proj[i], &frame->more_view_proj[i - 1], sizeof(block.light_view_proj[i]));
            block.texel[i] = frame->more_texel_world_size[i - 1];
        }
        for (uint32_t i = 0; i < 4; ++i) block.splits[i] = frame->split_far[i];
        block.params[0] = frame->normal_offset_texels;
        block.params[1] = static_cast<float>(frame->filter_radius);
        block.params[2] = static_cast<float>(frame->cascade_count);
        block.params[3] = 1.0f;
        block.map[1] = frame->blend;
        block.camera_pos[0] = frame->camera_position.x;
        block.camera_pos[1] = frame->camera_position.y;
        block.camera_pos[2] = frame->camera_position.z;
        block.camera_fwd[0] = frame->camera_forward.x;
        block.camera_fwd[1] = frame->camera_forward.y;
        block.camera_fwd[2] = frame->camera_forward.z;
    }
    std::span<std::byte> mapped = device_.map(shadow_blocks_[slot].handle());  // may throw: nothing has been modified yet
    std::memcpy(mapped.data(), &block, sizeof(block));
    active_group_ = slot;
    shadow_frame_written_ = true;
    shadow_frame_number_ = frame_number;
}

void ModelRenderer::draw_shadow(rhi::CommandList& cmd, const Mat4& light_view_proj, std::span<const ModelDraw> draws, ModelFrameStats& stats)
{
    if (!shadows_enabled_) {
        throw std::logic_error("ModelRenderer::draw_shadow: shadows are not enabled");
    }
    check_palettes_unfrozen(draws);  // before anything is recorded or written
    rhi::PipelineHandle bound{};
    rhi::BindGroupHandle bound_palette{};
    for (const ModelDraw& d : draws) {
        if (d.model == nullptr) {
            continue;
        }
        for (const GpuPlacement& placement : d.model->placements) {
            if (placement.mesh >= d.model->meshes.size()) {
                continue;
            }
            const Mat4 world = d.world * placement.local;
            const float det = determinant3(world);
            if (!finite_affine(world) || !std::isfinite(det) || std::fabs(det) < 1e-12f) {
                ++stats.instances_skipped;
                continue;
            }
            ShadowPush push{};
            push.light_view_proj = light_view_proj;
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 4; ++c) {
                    push.model_row[r][c] = world.at(r, c);
                }
            }
            for (const GpuPrimitive& prim : d.model->meshes[placement.mesh]) {
                if (!d.model->materials[prim.material].casts_sun_shadow) {
                    continue;  // authored MASK and BLEND surfaces do not cast (first slice)
                }
                const SkinPalette* palette = nullptr;
                if (prim.skinned) {
                    palette = d.palette != nullptr ? d.palette : &identity_palette_;
                    if (!skinning_enabled_ || !palette->valid() || palette->joint_count() < prim.joint_limit) {
                        ++stats.skinned_skipped;
                        continue;
                    }
                }
                const rhi::PipelineHandle pipeline = palette != nullptr ? skin_shadow_pipeline_ : shadow_pipeline_;
                if (pipeline != bound) {
                    cmd.bind_pipeline(pipeline);
                    bound = pipeline;
                    bound_palette = {};
                }
                if (palette != nullptr) {
                    const rhi::BindGroupHandle group = palette_group(*palette);
                    if (group != bound_palette) {
                        cmd.bind_group(0, group);
                        bound_palette = group;
                    }
                    ++stats.skinned_draws;
                }
                cmd.bind_vertex_buffer(prim.vertices.handle());
                cmd.bind_index_buffer(prim.indices.handle(), prim.index_type);
                cmd.push_constant_struct(push);
                cmd.draw_indexed(prim.index_count);
                ++stats.draw_calls;
            }
            ++stats.instances_drawn;
        }
    }
}

} // namespace saige::render
