#include "saige/gfx_scene/stage_model.hpp"

#include <algorithm>
#include <cmath>
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

/// A problem with what the asset says about one texture slot (a bad index, a missing or undecodable image). It is fatal for the base
/// color slot, as in 0.2; for the optional slots, which 0.2 never read, it leaves the slot neutral with a warning. A *device* failure
/// (GPU upload) is never a SlotProblem: it always fails the staging.
struct SlotProblem {
    std::string where;
    std::string why;
};

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
    const auto load_slot_texture = [&](const Material& material, std::size_t material_index, const char* slot_name, const TextureRef& ref,
                                       bool srgb) -> render::MaterialTextureSlot {
        if (!ref.valid()) {
            return {};
        }
        const std::string who = label(name, "material", material_index, material.name);
        if (static_cast<std::size_t>(ref.texture) >= asset.textures.size()) {
            throw SlotProblem{who, std::string(slot_name) + " texture index is out of range"};
        }
        const Texture& texture = asset.textures[static_cast<std::size_t>(ref.texture)];
        if (texture.image < 0 || static_cast<std::size_t>(texture.image) >= asset.images.size()) {
            throw SlotProblem{who, std::string(slot_name) + " texture has no usable source image"};
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
                throw SlotProblem{image_label, decoded.error};
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
            throw SlotProblem{image_label, "this build has no image decoder (SAIGE_GFX_IMAGE_DECODE is OFF)"};
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
    const auto slot_texture = [&](const Material& material, std::size_t material_index, const char* slot_name, const TextureRef& ref,
                                  bool srgb, bool optional) -> render::MaterialTextureSlot {
        try {
            return load_slot_texture(material, material_index, slot_name, ref, srgb);
        } catch (const SlotProblem& problem) {
            if (!optional) {
                fail(problem.where, problem.why);
            }
            warn_once(report, "an optional material texture could not be used and its slot is left neutral: " + problem.where + ": " + problem.why);
            return {};
        }
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
        // The authored alpha mode decides shadow casting, independently of how the surface is drawn (a BLEND drawn opaque because blending
        // is off still does not cast; a MASK with cutoff 0 still does not cast): only an opaque material does.
        data.casts_sun_shadow = m.alpha_mode == AlphaMode::Opaque;
        if (m.alpha_mode == AlphaMode::Mask) {
            // glTF renders a fragment when alpha >= cutoff, so a cutoff of 0 passes everything, which is exactly what the
            // shader does for 0 (no test); the shader discards only alpha < cutoff.
            data.alpha_cutoff = m.alpha_cutoff;
        } else if (m.alpha_mode == AlphaMode::Blend) {
            data.alpha_blend = true;  // the renderer decides: sorted blending only if it was asked for and the scene format can blend
            if (!models.alpha_blending()) {
                warn_once(report, "alpha blending is off (RendererSettings::alpha_blending): Blend materials draw as opaque, as in 0.2");
            } else if (!models.blending_supported()) {
                warn_once(report, "the scene's color format cannot blend: Blend materials draw as opaque");
            }
        }
        // Legacy (the default) stages what 0.2 staged: the base color texture only, so 0.2 content loads and looks as it did, including
        // content whose other textures could not be decoded. Extended and MetallicRoughness stage all five slots; an optional slot that
        // cannot be used stays neutral.
        const render::ModelShading shading = models.shading();
        const bool extended = shading != render::ModelShading::Legacy;
        const bool other_slots = m.normal_texture.valid() || m.metallic_roughness_texture.valid() || m.occlusion_texture.valid() || m.emissive_texture.valid();
        if (!extended && other_slots) {
            warn_once(report, "normal, metallic-roughness, occlusion and emissive textures are not sampled in 0.2 (base color only)");
        } else if (shading == render::ModelShading::Extended && m.normal_texture.valid()) {
            warn_once(report, "normal textures are uploaded and bound but not shaded yet");  // only MetallicRoughness shades them
        }
        render::MaterialTextures textures;
        textures[render::MaterialSlot::BaseColor] = slot_texture(m, i, "base color", m.base_color_texture, true, /*optional=*/false);
        if (extended) {
            textures[render::MaterialSlot::MetallicRoughness] = slot_texture(m, i, "metallic-roughness", m.metallic_roughness_texture, false, true);
            // The tangents are generated for TEXCOORD_0, and only TEXCOORD_0 is sampled. A normal map that names another coordinate
            // set cannot be shaded correctly (it would be read with the wrong UVs and a frame that does not belong to them), so
            // the slot is left empty and the surface keeps its geometric normal.
            TextureRef normal_ref = m.normal_texture;
            if (normal_ref.valid() && normal_ref.tex_coord != 0) {
                warn_once(report, "a normal texture that names TEXCOORD_" + std::to_string(normal_ref.tex_coord) +
                                      " is ignored: only TEXCOORD_0 is sampled, so the surface keeps its geometric normal");
                normal_ref = TextureRef{};
            }
            textures[render::MaterialSlot::Normal] = slot_texture(m, i, "normal", normal_ref, false, true);
            textures[render::MaterialSlot::Occlusion] = slot_texture(m, i, "occlusion", m.occlusion_texture, false, true);
            textures[render::MaterialSlot::Emissive] = slot_texture(m, i, "emissive", m.emissive_texture, true, true);
        }
        const std::string material_label = label(name, "material", i, m.name);
        model.materials.push_back(models.create_material(data, textures, material_label.c_str()));
    }
    // Primitives without a material use glTF's default: opaque white, single-sided.
    const uint32_t default_material = static_cast<uint32_t>(model.materials.size());
    model.materials.push_back(models.create_material({}, render::MaterialTextures{}, (std::string(name) + "/default material").c_str()));

    // ------------------------------------------------------------------------------------------ skin
    // One model, one skin: the first skinned instance's skin. Meshes that a skinned instance uses are staged skinned when the renderer can skin.
    int32_t model_skin = -1;
    std::vector<bool> mesh_is_skinned(asset.meshes.size(), false);
    for (const MeshInstance& inst : asset.instances) {
        if (inst.skin < 0 || inst.mesh < 0 || static_cast<std::size_t>(inst.mesh) >= asset.meshes.size()) continue;
        if (static_cast<std::size_t>(inst.skin) >= asset.skins.size()) fail(std::string(name), "a scene node refers to a skin that does not exist");
        if (model_skin < 0) model_skin = inst.skin;
        if (inst.skin == model_skin) mesh_is_skinned[static_cast<std::size_t>(inst.mesh)] = true;
    }
    const bool can_skin = models.skinning_enabled();
    uint32_t skin_joints = 0;
    if (model_skin >= 0) {
        skin_joints = static_cast<uint32_t>(asset.skins[static_cast<std::size_t>(model_skin)].joint_count());
        if (can_skin && skin_joints > render::kMaxSkinJoints) {
            fail(label(name, "skin", static_cast<std::size_t>(model_skin), asset.skins[static_cast<std::size_t>(model_skin)].name),
                 "has " + std::to_string(skin_joints) + " joints; GPU skinning supports at most " + std::to_string(render::kMaxSkinJoints));
        }
        if (!can_skin) warn_once(report, "skinning is off (RendererSettings::skinning): skinned meshes are staged rigid, in their bind pose");
    }

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
            const bool has_skin_data = !p.joints.empty() && p.joints.size() == std::size_t{vertex_count} * 4 && p.weights.size() == p.joints.size();
            const bool skinned = can_skin && mesh_is_skinned[mi] && has_skin_data;
            uint32_t joint_limit = 0;
            if (skinned) {
                for (std::size_t k = 0; k < p.joints.size(); ++k) {
                    if (p.weights[k] > 0.0f) joint_limit = std::max<uint32_t>(joint_limit, uint32_t{p.joints[k]} + 1u);
                }
                if (joint_limit > skin_joints) {
                    fail(where, "a vertex is bound to joint " + std::to_string(joint_limit - 1) + " but the skin has " + std::to_string(skin_joints) + " joints");
                }
            } else if (mesh_is_skinned[mi] && !has_skin_data && model_skin >= 0 && can_skin) {
                warn_once(report, "a skinned node instances a primitive without joints and weights: it is drawn rigid");
            }
            std::vector<render::SkinnedModelVertex> skinned_vertices(skinned ? vertex_count : 0);
            std::vector<render::ModelVertex> plain_vertices(skinned ? 0 : vertex_count);
            const auto vertex_at = [&](uint32_t v) -> render::ModelVertex& { return skinned ? skinned_vertices[v].base : plain_vertices[v]; };
            for (uint32_t v = 0; v < vertex_count; ++v) {
                render::ModelVertex& vtx = vertex_at(v);
                if (skinned) {
                    for (uint32_t c = 0; c < 4; ++c) {
                        skinned_vertices[v].joints[c] = p.joints[v * 4 + c];
                        skinned_vertices[v].weights[c] = p.weights[v * 4 + c];
                    }
                }
                vtx.position = {p.positions[v * 3], p.positions[v * 3 + 1], p.positions[v * 3 + 2]};
                vtx.normal = {p.normals[v * 3], p.normals[v * 3 + 1], p.normals[v * 3 + 2]};
                if (!p.uv0.empty()) {
                    vtx.uv[0] = p.uv0[v * 2];
                    vtx.uv[1] = p.uv0[v * 2 + 1];
                }
                if (!p.tangents.empty()) {  // absent tangents stay w = 0: the shader skips normal mapping there
                    for (uint32_t c = 0; c < 4; ++c) {
                        vtx.tangent[c] = p.tangents[v * 4 + c];
                    }
                }
            }
            if (p.tangents.empty() && p.material >= 0 && static_cast<std::size_t>(p.material) < asset.materials.size() &&
                asset.materials[static_cast<std::size_t>(p.material)].normal_texture.valid() &&
                asset.materials[static_cast<std::size_t>(p.material)].normal_texture.tex_coord == 0) {
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
            vb.size = skinned ? skinned_vertices.size() * sizeof(render::SkinnedModelVertex) : plain_vertices.size() * sizeof(render::ModelVertex);
            vb.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
            vb.debug_name = vname.c_str();
            rhi::BufferDesc ib;
            ib.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
            ib.debug_name = iname.c_str();
            try {
                prim.vertices = skinned ? registry.create_buffer(vb, std::as_bytes(std::span(skinned_vertices))) : registry.create_buffer(vb, std::as_bytes(std::span(plain_vertices)));
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
            prim.skinned = skinned;
            prim.joint_limit = joint_limit;
            if (skinned) ++report.skinned_primitives;
            {   // bounding sphere for sorting blended draws: the box center and the farthest vertex from it
                float lo[3] = {p.positions[0], p.positions[1], p.positions[2]};
                float hi[3] = {lo[0], lo[1], lo[2]};
                for (uint32_t v = 1; v < vertex_count; ++v) {
                    for (uint32_t c = 0; c < 3; ++c) {
                        lo[c] = std::min(lo[c], p.positions[v * 3 + c]);
                        hi[c] = std::max(hi[c], p.positions[v * 3 + c]);
                    }
                }
                const render::Float3 center{(lo[0] + hi[0]) * 0.5f, (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f};
                float radius2 = 0.0f;
                for (uint32_t v = 0; v < vertex_count; ++v) {
                    const float dx = p.positions[v * 3] - center.x, dy = p.positions[v * 3 + 1] - center.y, dz = p.positions[v * 3 + 2] - center.z;
                    radius2 = std::max(radius2, dx * dx + dy * dy + dz * dz);
                }
                prim.bounds_center = center;
                prim.bounds_radius = std::sqrt(radius2);
            }
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
        if (inst.skin >= 0 && inst.skin != model_skin) {
            warn_once(report, "a skinned instance of a second skin is not placed: one model supports one skin (one palette)");
            continue;
        }
        // A skinned instance is placed by its skin matrices; the node's own transform is irrelevant (the importer already made it the identity).
        const render::Mat4 world = inst.skin >= 0 && can_skin ? render::Mat4::identity() : inst.world;
        model.placements.push_back({static_cast<uint32_t>(inst.mesh), world});
    }
    model.skin_joint_count = report.skinned_primitives > 0 ? skin_joints : 0;
    if (model.placements.empty()) {
        warn_once(report, "the asset's scene places no meshes: it will draw nothing");
    }
    return model;
}

} // namespace saige::gfx
