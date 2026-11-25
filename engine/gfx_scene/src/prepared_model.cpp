#include "saige/gfx_scene/prepared_model.hpp"

#include "stage_util.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <span>

namespace saige::gfx {

using detail::fail;
using detail::label;
using detail::SlotProblem;
using detail::warn_once;

namespace {

using Clock = std::chrono::steady_clock;

double seconds_since(Clock::time_point start)
{
    return std::chrono::duration<double>(Clock::now() - start).count();
}

template <typename T>
std::vector<std::byte> bytes_of(const std::vector<T>& v)
{
    std::vector<std::byte> out(v.size() * sizeof(T));
    if (!out.empty()) std::memcpy(out.data(), v.data(), out.size());
    return out;
}

}  // namespace

StageCapabilities capabilities_of(render::SceneRenderer& renderer)
{
    const render::ModelRenderer& models = renderer.model_renderer();
    StageCapabilities c;
    c.shading = models.shading();
    c.skinning = models.skinning_enabled();
    c.alpha_blending = models.alpha_blending();
    c.blending_supported = models.blending_supported();
    return c;
}

rhi::TextureDesc texture_desc_of(const PreparedImage& image)
{
    rhi::TextureDesc td;
    td.format = image.srgb ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8_UNORM;
    td.width = image.width;
    td.height = image.height;
    td.mip_levels = image.levels;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    return td;
}

uint64_t PreparedModel::cpu_bytes() const noexcept
{
    uint64_t total = 0;
    for (const PreparedImage& i : images) total += i.data.size();
    for (const auto& mesh : meshes) {
        for (const PreparedPrimitive& p : mesh) total += p.vertices.size() + p.indices.size();
    }
    return total;
}

PreparedModel prepare_model(const GltfAsset& asset, std::string_view name, const StageCapabilities& caps, const StageSettings& settings)
{
    const auto started = Clock::now();
    PreparedModel out;
    out.name = std::string(name);
    out.capabilities = caps;
    StageReport& report = out.report;

    // ------------------------------------------------------------------------------------------ images and slots
    // One image per (source image, color space): the same image used as base color (sRGB) and as a linear data texture is decoded twice, because the format decides how the
    // GPU reads it. Each slot names the sampler its glTF texture names; identical sampler descriptions share one device sampler through the registry at upload.
    std::map<std::pair<int32_t, bool>, int32_t> image_index;
    const auto prepare_slot = [&](const Material& material, std::size_t material_index, const char* slot_name, const TextureRef& ref, bool srgb) -> PreparedSlot {
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
        int32_t index = -1;
        const std::pair<int32_t, bool> key{texture.image, srgb};
        if (const auto it = image_index.find(key); it != image_index.end()) {
            index = it->second;
        } else {
            const Image& image = asset.images[static_cast<std::size_t>(texture.image)];
            const std::string image_label = label(name, "image", static_cast<std::size_t>(texture.image), image.name);
#if defined(SAIGE_HAS_IMAGE_DECODE)
            const auto decode_started = Clock::now();
            const ImageDecodeResult decoded = decode_image(image.bytes, settings.image_limits);
            if (!decoded.ok()) {
                throw SlotProblem{image_label, decoded.error};
            }
            MipChain chain = generate_mips(*decoded.image, srgb ? ColorSpace::Srgb : ColorSpace::Linear);
            out.decode_seconds += seconds_since(decode_started);
            PreparedImage prepared;
            prepared.label = image_label;
            prepared.srgb = srgb;
            prepared.width = chain.width;
            prepared.height = chain.height;
            prepared.levels = chain.levels;
            prepared.data = std::move(chain.data);
            index = static_cast<int32_t>(out.images.size());
            out.images.push_back(std::move(prepared));
            image_index.emplace(key, index);
#else
            (void)settings;
            throw SlotProblem{image_label, "this build has no image decoder (SAIGE_GFX_IMAGE_DECODE is OFF)"};
#endif
        }
        if (ref.tex_coord != 0) {
            warn_once(report, "only TEXCOORD_0 is sampled: a texture that names another coordinate set reads TEXCOORD_0");
        }
        PreparedSlot slot;
        slot.image = index;
        slot.sampler = to_sampler_desc(texture.sampler >= 0 && static_cast<std::size_t>(texture.sampler) < asset.samplers.size()
                                           ? asset.samplers[static_cast<std::size_t>(texture.sampler)]
                                           : Sampler{},
                                       settings.max_anisotropy);
        return slot;
    };
    const auto slot_texture = [&](const Material& material, std::size_t material_index, const char* slot_name, const TextureRef& ref, bool srgb, bool optional) -> PreparedSlot {
        try {
            return prepare_slot(material, material_index, slot_name, ref, srgb);
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
        PreparedMaterial pm;
        render::MaterialData& data = pm.data;
        std::memcpy(data.base_color, m.base_color, sizeof(data.base_color));
        std::memcpy(data.emissive, m.emissive, sizeof(data.emissive));
        data.metallic = m.metallic;
        data.roughness = m.roughness;
        data.normal_scale = m.normal_scale;
        data.occlusion_strength = m.occlusion_strength;
        data.double_sided = m.double_sided;
        // The authored alpha mode decides shadow casting, independently of how the surface is drawn: opaque and MASK materials cast (a MASK
        // through the alpha-tested variant, using its cutoff), a BLEND does not even when it is drawn opaque because blending is off.
        data.casts_sun_shadow = m.alpha_mode != AlphaMode::Blend;
        if (m.alpha_mode == AlphaMode::Mask) {
            // glTF renders a fragment when alpha >= cutoff, so a cutoff of 0 passes everything, which is exactly what the
            // shader does for 0 (no test); the shader discards only alpha < cutoff.
            data.alpha_cutoff = m.alpha_cutoff;
        } else if (m.alpha_mode == AlphaMode::Blend) {
            data.alpha_blend = true;  // the renderer decides: sorted blending only if it was asked for and the scene format can blend
            if (!caps.alpha_blending) {
                warn_once(report, "alpha blending is off (RendererSettings::alpha_blending): Blend materials draw as opaque, as in 0.2");
            } else if (!caps.blending_supported) {
                warn_once(report, "the scene's color format cannot blend: Blend materials draw as opaque");
            }
        }
        // Legacy (the default) stages what 0.2 staged: the base color texture only, so 0.2 content loads and looks as it did, including
        // content whose other textures could not be decoded. Extended and MetallicRoughness stage all five slots; an optional slot that
        // cannot be used stays neutral.
        const bool extended = caps.shading != render::ModelShading::Legacy;
        const bool other_slots = m.normal_texture.valid() || m.metallic_roughness_texture.valid() || m.occlusion_texture.valid() || m.emissive_texture.valid();
        if (!extended && other_slots) {
            warn_once(report, "normal, metallic-roughness, occlusion and emissive textures are not sampled in 0.2 (base color only)");
        } else if (caps.shading == render::ModelShading::Extended && m.normal_texture.valid()) {
            warn_once(report, "normal textures are uploaded and bound but not shaded yet");  // only MetallicRoughness shades them
        }
        pm.slots[static_cast<uint32_t>(render::MaterialSlot::BaseColor)] = slot_texture(m, i, "base color", m.base_color_texture, true, /*optional=*/false);
        if (extended) {
            pm.slots[static_cast<uint32_t>(render::MaterialSlot::MetallicRoughness)] = slot_texture(m, i, "metallic-roughness", m.metallic_roughness_texture, false, true);
            // The tangents are generated for TEXCOORD_0, and only TEXCOORD_0 is sampled. A normal map that names another coordinate
            // set cannot be shaded correctly (it would be read with the wrong UVs and a frame that does not belong to them), so
            // the slot is left empty and the surface keeps its geometric normal.
            TextureRef normal_ref = m.normal_texture;
            if (normal_ref.valid() && normal_ref.tex_coord != 0) {
                warn_once(report, "a normal texture that names TEXCOORD_" + std::to_string(normal_ref.tex_coord) +
                                      " is ignored: only TEXCOORD_0 is sampled, so the surface keeps its geometric normal");
                normal_ref = TextureRef{};
            }
            pm.slots[static_cast<uint32_t>(render::MaterialSlot::Normal)] = slot_texture(m, i, "normal", normal_ref, false, true);
            pm.slots[static_cast<uint32_t>(render::MaterialSlot::Occlusion)] = slot_texture(m, i, "occlusion", m.occlusion_texture, false, true);
            pm.slots[static_cast<uint32_t>(render::MaterialSlot::Emissive)] = slot_texture(m, i, "emissive", m.emissive_texture, true, true);
        }
        pm.label = label(name, "material", i, m.name);
        out.materials.push_back(std::move(pm));
    }
    // Primitives without a material use glTF's default: opaque white, single-sided.
    const uint32_t default_material = static_cast<uint32_t>(out.materials.size());
    {
        PreparedMaterial pm;
        pm.label = std::string(name) + "/default material";
        out.materials.push_back(std::move(pm));
    }

    // ------------------------------------------------------------------------------------------ skin
    // One model, one skin: the first skinned instance's skin. Meshes that a skinned instance uses are packed skinned when the renderer can skin.
    int32_t model_skin = -1;
    std::vector<bool> mesh_is_skinned(asset.meshes.size(), false);
    for (const MeshInstance& inst : asset.instances) {
        if (inst.skin < 0 || inst.mesh < 0 || static_cast<std::size_t>(inst.mesh) >= asset.meshes.size()) continue;
        if (static_cast<std::size_t>(inst.skin) >= asset.skins.size()) fail(std::string(name), "a scene node refers to a skin that does not exist");
        if (model_skin < 0) model_skin = inst.skin;
        if (inst.skin == model_skin) mesh_is_skinned[static_cast<std::size_t>(inst.mesh)] = true;
    }
    const bool can_skin = caps.skinning;
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
        std::vector<PreparedPrimitive> primitives;
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
            // Per-joint bind-space boxes for conservative per-pose culling. A vertex whose weights do not sum to one is not a convex combination, so no bound would hold:
            // then the primitive keeps no joint bounds and is never culled.
            std::vector<render::JointBounds> joint_bounds;
            if (skinned) {
                joint_bounds.assign(joint_limit, render::JointBounds{});
                for (uint32_t v = 0; v < vertex_count && !joint_bounds.empty(); ++v) {
                    float sum = 0.0f;
                    for (uint32_t c = 0; c < 4; ++c) sum += p.weights[v * 4 + c];
                    if (!(std::fabs(sum - 1.0f) <= 1e-3f)) {
                        joint_bounds.clear();
                        break;
                    }
                    const render::Float3 position{p.positions[v * 3], p.positions[v * 3 + 1], p.positions[v * 3 + 2]};
                    for (uint32_t c = 0; c < 4; ++c) {
                        if (!(p.weights[v * 4 + c] > 0.0f)) continue;
                        render::JointBounds& jb = joint_bounds[p.joints[v * 4 + c]];
                        if (!jb.used) {
                            jb.min = jb.max = position;
                            jb.used = true;
                        } else {
                            jb.min = {std::min(jb.min.x, position.x), std::min(jb.min.y, position.y), std::min(jb.min.z, position.z)};
                            jb.max = {std::max(jb.max.x, position.x), std::max(jb.max.y, position.y), std::max(jb.max.z, position.z)};
                        }
                    }
                }
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
            PreparedPrimitive prim;
            prim.where = where;
            prim.vertices = skinned ? bytes_of(skinned_vertices) : bytes_of(plain_vertices);
            if (vertex_count <= std::numeric_limits<uint16_t>::max()) {
                std::vector<uint16_t> narrow;
                narrow.reserve(indices.size());
                for (const uint32_t index : indices) narrow.push_back(static_cast<uint16_t>(index));
                prim.indices = bytes_of(narrow);
                prim.index_type = rhi::IndexType::Uint16;
            } else {
                prim.indices = bytes_of(indices);
                prim.index_type = rhi::IndexType::Uint32;
            }
            prim.index_count = static_cast<uint32_t>(indices.size());
            prim.skinned = skinned;
            prim.joint_limit = joint_limit;
            prim.joint_bounds = std::move(joint_bounds);
            if (skinned) ++report.skinned_primitives;
            {   // bounding sphere for sorting blended draws and culling: the box center and the farthest vertex from it
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
            prim.material = (p.material >= 0 && static_cast<std::size_t>(p.material) < asset.materials.size()) ? static_cast<uint32_t>(p.material) : default_material;
            report.vertex_bytes += prim.vertices.size();
            report.index_bytes += prim.indices.size();
            ++report.primitives;
            primitives.push_back(std::move(prim));
        }
        out.meshes.push_back(std::move(primitives));
    }

    // ------------------------------------------------------------------------------------------ placements
    for (const MeshInstance& inst : asset.instances) {
        if (inst.mesh < 0 || static_cast<std::size_t>(inst.mesh) >= out.meshes.size()) {
            fail(std::string(name), "a scene node refers to a mesh that does not exist");
        }
        if (inst.skin >= 0 && inst.skin != model_skin) {
            warn_once(report, "a skinned instance of a second skin is not placed: one model supports one skin (one palette)");
            continue;
        }
        // A skinned instance is placed by its skin matrices; the node's own transform is irrelevant (the importer already made it the identity).
        const render::Mat4 world = inst.skin >= 0 && can_skin ? render::Mat4::identity() : inst.world;
        out.placements.push_back({static_cast<uint32_t>(inst.mesh), world});
    }
    out.skin_joint_count = report.skinned_primitives > 0 ? skin_joints : 0;
    if (out.placements.empty()) {
        warn_once(report, "the asset's scene places no meshes: it will draw nothing");
    }
    out.prepare_seconds = seconds_since(started);
    report.prepare_seconds = out.prepare_seconds;
    return out;
}

render::GpuModel upload_model(render::SceneRenderer& renderer, const PreparedModel& prepared, StageReport* report_out, double* upload_seconds)
{
    const auto started = Clock::now();
    if (!(capabilities_of(renderer) == prepared.capabilities)) {
        throw std::invalid_argument("upload_model: '" + prepared.name + "' was prepared for a renderer with different capabilities (shading mode, skinning or alpha blending)");
    }
    StageReport local;
    StageReport& report = report_out != nullptr ? *report_out : local;
    report = prepared.report;
    report.textures = 0;
    report.texture_bytes = 0;
    render::GpuResourceRegistry& registry = renderer.registry();
    render::ModelRenderer& models = renderer.model_renderer();
    render::GpuModel model;
    model.name = prepared.name;

    // One GPU texture per prepared image, created at its first use by a slot (the order the staging always created them in).
    std::vector<render::TextureRef> textures(prepared.images.size());
    std::vector<bool> created(prepared.images.size(), false);
    const auto texture_for = [&](int32_t index) -> render::TextureRef {
        const std::size_t i = static_cast<std::size_t>(index);
        if (!created[i]) {
            const PreparedImage& image = prepared.images[i];
            rhi::TextureDesc td = texture_desc_of(image);
            const std::string debug = image.label + (image.srgb ? " (sRGB)" : " (linear)");
            td.debug_name = debug.c_str();
            try {
                textures[i] = registry.create_texture(td, std::as_bytes(std::span(image.data)));
            } catch (const std::exception& e) {
                fail(image.label, std::string("GPU upload failed: ") + e.what());
            }
            ++report.textures;
            report.texture_bytes += textures[i].bytes();
            created[i] = true;
        }
        return textures[i];
    };
    for (const PreparedMaterial& pm : prepared.materials) {
        render::MaterialTextures slots;
        for (uint32_t s = 0; s < render::kMaterialSlotCount; ++s) {
            if (pm.slots[s].image >= 0) {
                slots.slots[s] = {texture_for(pm.slots[s].image), registry.get_sampler(pm.slots[s].sampler)};
            }
        }
        model.materials.push_back(models.create_material(pm.data, slots, pm.label.c_str()));
    }
    for (const auto& mesh : prepared.meshes) {
        std::vector<render::GpuPrimitive> primitives;
        for (const PreparedPrimitive& p : mesh) {
            render::GpuPrimitive prim;
            const std::string vname = p.where + " vertices";
            const std::string iname = p.where + " indices";
            rhi::BufferDesc vb;
            vb.size = p.vertices.size();
            vb.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
            vb.debug_name = vname.c_str();
            rhi::BufferDesc ib;
            ib.size = p.indices.size();
            ib.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
            ib.debug_name = iname.c_str();
            try {
                prim.vertices = registry.create_buffer(vb, std::span<const std::byte>(p.vertices));
                prim.indices = registry.create_buffer(ib, std::span<const std::byte>(p.indices));
            } catch (const std::exception& e) {
                fail(p.where, std::string("GPU upload failed: ") + e.what());
            }
            prim.index_type = p.index_type;
            prim.index_count = p.index_count;
            prim.skinned = p.skinned;
            prim.joint_limit = p.joint_limit;
            prim.joint_bounds = p.joint_bounds;
            prim.bounds_center = p.bounds_center;
            prim.bounds_radius = p.bounds_radius;
            prim.material = p.material;
            primitives.push_back(std::move(prim));
        }
        model.meshes.push_back(std::move(primitives));
    }
    model.placements = prepared.placements;
    model.skin_joint_count = prepared.skin_joint_count;
    const double elapsed = seconds_since(started);
    report.upload_seconds = elapsed;
    if (upload_seconds != nullptr) *upload_seconds = elapsed;
    return model;
}

} // namespace saige::gfx
