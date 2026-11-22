#include "saige/render/model_renderer.hpp"

#include "saige/render/env_brdf.hpp"

#include "saige/shaders/scene_model.spv.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
    float flags[4];  ///< x: shading mode (0 Legacy, 1 Extended, 2 MetallicRoughness)
};

/// Material uniform block (set 0, binding 10), matching `Material` in the shader.
struct MaterialBlock {
    float base_color[4];
    float emissive_cutoff[4];
    float metallic_roughness_normal_occlusion[4];  ///< metallic, roughness, normal scale, occlusion strength
    float flags[4];                                ///< x: 1 when the material has a normal texture
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
    ub.size = 256;  // a multiple of every uniform alignment; only `size` bytes are bound
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

ModelRenderer::ModelRenderer(rhi::Device& device, GpuResourceRegistry& registry, rhi::Format color_format,
                             rhi::Format depth_format, const ModelRenderSettings& settings)
    : device_(device), registry_(registry), shading_(settings.shading)
{
    using namespace saige::shaders;
    vs_ = device_.create_shader({rhi::ShaderStage::Vertex, words(scene_model_vs_main), "VSMain", "SceneModel.VS"});
    ps_ = device_.create_shader({rhi::ShaderStage::Pixel, words(scene_model_ps_main), "PSMain", "SceneModel.PS"});

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
    static const char* const names[4] = {"SceneModel.CCW.Back", "SceneModel.CCW.None", "SceneModel.CW.Back", "SceneModel.CW.None"};
    for (int mirrored = 0; mirrored < 2; ++mirrored) {
        for (int two_sided = 0; two_sided < 2; ++two_sided) {
            pd.front_face = mirrored != 0 ? rhi::FrontFace::Clockwise : rhi::FrontFace::CounterClockwise;
            pd.cull_mode = two_sided != 0 ? rhi::CullMode::None : rhi::CullMode::Back;
            pd.debug_name = names[mirrored * 2 + two_sided];
            pipelines_[mirrored * 2 + two_sided] = device_.create_graphics_pipeline(pd);
        }
    }

    // Immutable scene constants: the same sun, ambient and fog as the 0.1 cube and ground shaders.
    const float sun_len = std::sqrt(0.40f * 0.40f + 0.55f * 0.55f + 0.73f * 0.73f);
    SceneConstants sc{};
    sc.fog_color_density[0] = settings.sky.r;
    sc.fog_color_density[1] = settings.sky.g;
    sc.fog_color_density[2] = settings.sky.b;
    sc.fog_color_density[3] = settings.fog_density;
    sc.sun_direction[0] = -0.40f / sun_len;
    sc.sun_direction[1] = -0.55f / sun_len;
    sc.sun_direction[2] = 0.73f / sun_len;
    sc.sun_color[0] = 1.00f;
    sc.sun_color[1] = 0.95f;
    sc.sun_color[2] = 0.85f;
    sc.ambient_ground[0] = 0.20f;
    sc.ambient_ground[1] = 0.18f;
    sc.ambient_ground[2] = 0.16f;
    sc.flags[0] = static_cast<float>(static_cast<uint8_t>(shading_));  // 0 Legacy, 1 Extended, 2 MetallicRoughness
    BufferRef constants_buffer;
    make_uniform(device_, registry_, constants_buffer, &sc, sizeof(sc), "ModelSceneConstants");
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

    // The split-sum environment BRDF table (metallic-roughness shading's ambient specular). Legacy shading never reads it, so
    // it gets one zero texel instead of paying for the integration.
    {
        const bool needed = settings.shading == ModelShading::MetallicRoughness;
        const EnvBrdfTable zero{1, {0, 0}};
        const EnvBrdfTable& table = needed ? default_env_brdf_table() : zero;
        rhi::TextureDesc d;
        d.format = rhi::Format::RG16_FLOAT;
        d.width = d.height = table.size;
        d.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
        d.debug_name = "ModelEnvBrdf";
        env_brdf_ = registry_.create_texture(d, std::as_bytes(std::span(table.texels)));
        rhi::SamplerDesc s;
        s.address_u = s.address_v = rhi::AddressMode::ClampToEdge;
        s.mipmap_mode = rhi::MipmapMode::Nearest;
        s.max_lod = 0.0f;
        env_brdf_sampler_ = registry_.get_sampler(s);
    }

    BindGroupBinding constant_bindings[3];
    constant_bindings[0].binding = 0;
    constant_bindings[0].buffer = constants_buffer;
    constant_bindings[0].size = sizeof(sc);
    constant_bindings[1].binding = 1;
    constant_bindings[1].texture = env_brdf_;
    constant_bindings[2].binding = 2;
    constant_bindings[2].sampler = env_brdf_sampler_;
    constants_group_ = registry_.create_bind_group(constants_layout_, constant_bindings, "ModelSceneConstants");
}

ModelRenderer::~ModelRenderer()
{
    constants_group_.reset();
    white_srgb_.reset();
    white_linear_.reset();
    flat_normal_.reset();
    env_brdf_.reset();
    env_brdf_sampler_.reset();
    default_sampler_.reset();
    for (const rhi::PipelineHandle p : pipelines_) {
        device_.destroy(p);
    }
    device_.destroy(ps_);
    device_.destroy(vs_);
    device_.destroy(material_layout_);
    device_.destroy(constants_layout_);
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
    return material;
}

rhi::PipelineHandle ModelRenderer::pipeline_for(bool mirrored, bool double_sided) const noexcept
{
    return pipelines_[(mirrored ? 2 : 0) + (double_sided ? 1 : 0)];
}

void ModelRenderer::draw(rhi::CommandList& cmd, const Mat4& view_proj, Float3 camera_position, std::span<const ModelDraw> draws,
                         ModelFrameStats& stats)
{
    rhi::PipelineHandle bound{};
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

            for (const GpuPrimitive& prim : d.model->meshes[placement.mesh]) {
                const GpuMaterial& material = d.model->materials[prim.material];
                const rhi::PipelineHandle pipeline = pipeline_for(mirrored, material.double_sided);
                if (pipeline != bound) {
                    cmd.bind_pipeline(pipeline);
                    cmd.bind_group(1, constants_group_.handle());  // a new pipeline starts with nothing bound
                    bound = pipeline;
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
}

} // namespace saige::render
