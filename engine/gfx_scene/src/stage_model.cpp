#include "saige/gfx_scene/stage_model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace saige::gfx {

namespace {

void warn_once(StageReport& report, std::string message)
{
    if (std::find(report.warnings.begin(), report.warnings.end(), message) == report.warnings.end()) {
        report.warnings.push_back(std::move(message));
    }
}

std::string label(std::string_view name, const std::string& kind, std::size_t index, const std::string& item_name)
{
    std::string out(name);
    out += '/';
    out += kind;
    out += '[' + std::to_string(index) + ']';
    if (!item_name.empty()) {
        out += " '" + item_name + "'";
    }
    return out;
}

[[noreturn]] void fail(const std::string& what, const std::string& why)
{
    throw std::runtime_error(what + ": " + why);
}

} // namespace

rhi::SamplerDesc to_sampler_desc(const Sampler& s, float max_anisotropy)
{
    rhi::SamplerDesc d;
    d.mag_filter = s.mag_filter == 9728 ? rhi::Filter::Nearest : rhi::Filter::Linear;
    bool mips = true;
    switch (s.min_filter) {
    case 9728: d.min_filter = rhi::Filter::Nearest; mips = false; break;
    case 9729: d.min_filter = rhi::Filter::Linear; mips = false; break;
    case 9984: d.min_filter = rhi::Filter::Nearest; d.mipmap_mode = rhi::MipmapMode::Nearest; break;
    case 9985: d.min_filter = rhi::Filter::Linear; d.mipmap_mode = rhi::MipmapMode::Nearest; break;
    case 9986: d.min_filter = rhi::Filter::Nearest; d.mipmap_mode = rhi::MipmapMode::Linear; break;
    case 9987:
    default: d.min_filter = rhi::Filter::Linear; d.mipmap_mode = rhi::MipmapMode::Linear; break;
    }
    if (!mips) {
        d.max_lod = 0.0f;  // a min filter without mipmaps samples only the base level
    }
    const auto wrap = [](uint32_t mode) {
        switch (mode) {
        case 33071: return rhi::AddressMode::ClampToEdge;
        case 33648: return rhi::AddressMode::MirroredRepeat;
        default: return rhi::AddressMode::Repeat;  // 10497, and anything unknown
        }
    };
    d.address_u = wrap(s.wrap_s);
    d.address_v = wrap(s.wrap_t);
    if (mips && d.min_filter == rhi::Filter::Linear && d.mag_filter == rhi::Filter::Linear && max_anisotropy > 1.0f) {
        d.max_anisotropy = std::min(max_anisotropy, 16.0f);
    }
    return d;
}

render::GpuModel stage_model(render::SceneRenderer& renderer, const GltfAsset& asset, std::string_view name,
                             const StageSettings& settings, StageReport* report_out)
{
    StageReport local;
    StageReport& report = report_out != nullptr ? *report_out : local;
    render::GpuResourceRegistry& registry = renderer.registry();
    render::ModelRenderer& models = renderer.model_renderer();
    render::GpuModel model;
    model.name = std::string(name);

    // ------------------------------------------------------------------------------------------ textures
    // One GPU texture per (source image, color space): the same image used as base color (sRGB) and as a linear data
    // texture is decoded and uploaded twice, because the format decides how the GPU reads it. Each slot gets the sampler
    // its glTF texture names; identical sampler descriptions share one device sampler through the registry.
    std::map<std::pair<int32_t, bool>, render::TextureRef> textures_by_image;
    const auto slot_texture = [&](const Material& material, std::size_t material_index, const char* slot_name, const TextureRef& ref,
                                  bool srgb) -> render::MaterialTextureSlot {
        if (!ref.valid()) {
            return {};
        }
        const std::string who = label(name, "material", material_index, material.name);
        if (static_cast<std::size_t>(ref.texture) >= asset.textures.size()) {
            fail(who, std::string(slot_name) + " texture index is out of range");
        }
        const Texture& texture = asset.textures[static_cast<std::size_t>(ref.texture)];
        if (texture.image < 0 || static_cast<std::size_t>(texture.image) >= asset.images.size()) {
            fail(who, std::string(slot_name) + " texture has no usable source image");
        }
        render::TextureRef tex;
        const std::pair<int32_t, bool> key{texture.image, srgb};
        if (const auto it = textures_by_image.find(key); it != textures_by_image.end()) {
            tex = it->second;
        } else {
            const Image& image = asset.images[static_cast<std::size_t>(texture.image)];
            const std::string image_label = label(name, "image", static_cast<std::size_t>(texture.image), image.name);
#if defined(SAIGE_HAS_IMAGE_DECODE)
            const ImageDecodeResult decoded = decode_image(image.bytes, settings.image_limits);
            if (!decoded.ok()) {
                fail(image_label, decoded.error);
            }
            const MipChain chain = generate_mips(*decoded.image, srgb ? ColorSpace::Srgb : ColorSpace::Linear);
            rhi::TextureDesc td;
            td.format = srgb ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8_UNORM;
            td.width = chain.width;
            td.height = chain.height;
            td.mip_levels = chain.levels;
            td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
            const std::string debug = image_label + (srgb ? " (sRGB)" : " (linear)");
            td.debug_name = debug.c_str();
            try {
                tex = registry.create_texture(td, std::as_bytes(std::span(chain.data)));
            } catch (const std::exception& e) {
                fail(image_label, std::string("GPU upload failed: ") + e.what());
            }
            ++report.textures;
            report.texture_bytes += tex.bytes();
            textures_by_image.emplace(key, tex);
#else
            (void)settings;
            fail(image_label, "this build has no image decoder (SAIGE_GFX_IMAGE_DECODE is OFF)");
#endif
        }
        if (ref.tex_coord != 0) {
            warn_once(report, "only TEXCOORD_0 is sampled: a texture that names another coordinate set reads TEXCOORD_0");
        }
        render::SamplerRef sampler;
        if (texture.sampler >= 0 && static_cast<std::size_t>(texture.sampler) < asset.samplers.size()) {
            sampler = registry.get_sampler(to_sampler_desc(asset.samplers[static_cast<std::size_t>(texture.sampler)], settings.max_anisotropy));
        } else {
            sampler = registry.get_sampler(to_sampler_desc(Sampler{}, settings.max_anisotropy));
        }
        return {tex, sampler};
    };

    // ------------------------------------------------------------------------------------------ materials
    for (std::size_t i = 0; i < asset.materials.size(); ++i) {
        const Material& m = asset.materials[i];
        render::MaterialData data;
        std::memcpy(data.base_color, m.base_color, sizeof(data.base_color));
        std::memcpy(data.emissive, m.emissive, sizeof(data.emissive));
        data.metallic = m.metallic;
        data.roughness = m.roughness;
        data.normal_scale = m.normal_scale;
        data.occlusion_strength = m.occlusion_strength;
        data.double_sided = m.double_sided;
        if (m.alpha_mode == AlphaMode::Mask) {
            // glTF renders a fragment when alpha >= cutoff, so a cutoff of 0 passes everything, which is exactly what the
            // shader does for 0 (no test); the shader discards only alpha < cutoff.
            data.alpha_cutoff = m.alpha_cutoff;
        } else if (m.alpha_mode == AlphaMode::Blend) {
            warn_once(report, "alpha blending is not implemented in 0.2: Blend materials draw as opaque");
        }
        render::MaterialTextures textures;
        textures[render::MaterialSlot::BaseColor] = slot_texture(m, i, "base color", m.base_color_texture, true);
        textures[render::MaterialSlot::MetallicRoughness] = slot_texture(m, i, "metallic-roughness", m.metallic_roughness_texture, false);
        textures[render::MaterialSlot::Normal] = slot_texture(m, i, "normal", m.normal_texture, false);
        textures[render::MaterialSlot::Occlusion] = slot_texture(m, i, "occlusion", m.occlusion_texture, false);
        textures[render::MaterialSlot::Emissive] = slot_texture(m, i, "emissive", m.emissive_texture, true);
        const std::string material_label = label(name, "material", i, m.name);
        model.materials.push_back(models.create_material(data, textures, material_label.c_str()));
    }
    // Primitives without a material use glTF's default: opaque white, single-sided.
    const uint32_t default_material = static_cast<uint32_t>(model.materials.size());
    model.materials.push_back(models.create_material({}, render::MaterialTextures{}, (std::string(name) + "/default material").c_str()));

    // ------------------------------------------------------------------------------------------ meshes
    for (std::size_t mi = 0; mi < asset.meshes.size(); ++mi) {
        const Mesh& mesh = asset.meshes[mi];
        std::vector<render::GpuPrimitive> primitives;
        for (std::size_t pi = 0; pi < mesh.primitives.size(); ++pi) {
            const Primitive& p = mesh.primitives[pi];
            const std::string where = label(name, "mesh", mi, mesh.name) + " primitive " + std::to_string(pi);
            const uint32_t vertex_count = p.vertex_count();
            if (vertex_count == 0 || p.normals.size() != p.positions.size()) {
                fail(where, "has no vertices or mismatched normals");
            }
            if (!p.uv0.empty() && p.uv0.size() != std::size_t{vertex_count} * 2) {
                fail(where, "has a texture coordinate count that does not match its vertices");
            }
            if (!p.tangents.empty() && p.tangents.size() != std::size_t{vertex_count} * 4) {
                fail(where, "has a tangent count that does not match its vertices");
            }
            std::vector<render::ModelVertex> vertices(vertex_count);
            for (uint32_t v = 0; v < vertex_count; ++v) {
                vertices[v].position = {p.positions[v * 3], p.positions[v * 3 + 1], p.positions[v * 3 + 2]};
                vertices[v].normal = {p.normals[v * 3], p.normals[v * 3 + 1], p.normals[v * 3 + 2]};
                if (!p.uv0.empty()) {
                    vertices[v].uv[0] = p.uv0[v * 2];
                    vertices[v].uv[1] = p.uv0[v * 2 + 1];
                }
                if (!p.tangents.empty()) {  // absent tangents stay w = 0: the shader skips normal mapping there
                    for (uint32_t c = 0; c < 4; ++c) {
                        vertices[v].tangent[c] = p.tangents[v * 4 + c];
                    }
                }
            }
            if (p.tangents.empty() && p.material >= 0 && static_cast<std::size_t>(p.material) < asset.materials.size() &&
                asset.materials[static_cast<std::size_t>(p.material)].normal_texture.valid()) {
                warn_once(report, "a primitive with a normal texture has no tangents (it has no texture coordinates): its normal map is ignored");
            }
            std::vector<uint32_t> indices = p.indices;
            if (indices.empty()) {  // a non-indexed triangle list
                indices.resize(vertex_count);
                for (uint32_t i = 0; i < vertex_count; ++i) indices[i] = i;
            }
            if (indices.size() % 3 != 0) {
                fail(where, "index count is not a multiple of 3");
            }
            for (const uint32_t index : indices) {
                if (index >= vertex_count) {
                    fail(where, "an index refers past the last vertex");
                }
            }
            render::GpuPrimitive prim;
            const std::string vname = where + " vertices";
            const std::string iname = where + " indices";
            rhi::BufferDesc vb;
            vb.size = vertices.size() * sizeof(render::ModelVertex);
            vb.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
            vb.debug_name = vname.c_str();
            rhi::BufferDesc ib;
            ib.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
            ib.debug_name = iname.c_str();
            try {
                prim.vertices = registry.create_buffer(vb, std::as_bytes(std::span(vertices)));
                if (vertex_count <= std::numeric_limits<uint16_t>::max()) {
                    std::vector<uint16_t> narrow;
                    narrow.reserve(indices.size());
                    for (const uint32_t index : indices) narrow.push_back(static_cast<uint16_t>(index));
                    ib.size = narrow.size() * sizeof(uint16_t);
                    prim.indices = registry.create_buffer(ib, std::as_bytes(std::span(narrow)));
                    prim.index_type = rhi::IndexType::Uint16;
                } else {
                    ib.size = indices.size() * sizeof(uint32_t);
                    prim.indices = registry.create_buffer(ib, std::as_bytes(std::span(indices)));
                    prim.index_type = rhi::IndexType::Uint32;
                }
            } catch (const std::exception& e) {
                fail(where, std::string("GPU upload failed: ") + e.what());
            }
            prim.index_count = static_cast<uint32_t>(indices.size());
            prim.material = (p.material >= 0 && static_cast<std::size_t>(p.material) < asset.materials.size())
                                ? static_cast<uint32_t>(p.material)
                                : default_material;
            report.vertex_bytes += vb.size;
            report.index_bytes += ib.size;
            ++report.primitives;
            primitives.push_back(std::move(prim));
        }
        model.meshes.push_back(std::move(primitives));
    }

    // ------------------------------------------------------------------------------------------ placements
    for (const MeshInstance& inst : asset.instances) {
        if (inst.mesh < 0 || static_cast<std::size_t>(inst.mesh) >= model.meshes.size()) {
            fail(std::string(name), "a scene node refers to a mesh that does not exist");
        }
        model.placements.push_back({static_cast<uint32_t>(inst.mesh), inst.world});
    }
    if (model.placements.empty()) {
        warn_once(report, "the asset's scene places no meshes: it will draw nothing");
    }
    return model;
}

} // namespace saige::gfx
