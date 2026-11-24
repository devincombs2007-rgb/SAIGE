// SAIGE gfx_scene — turns an imported glTF asset (CPU data from gfx_assets) into a GpuModel the renderer can draw.
//
// Staging is all-or-nothing. Every GPU resource is created through the renderer's registry and held by shared references;
// if any step fails (an undecodable image, a device error) the function throws and the references it created are
// released as the stack unwinds, so the caller keeps whatever scene it was already showing. A successful result is a
// complete candidate: activating it is moving it into place, and dropping the previous one retires its resources on the
// device's completion timeline.
//
// What is uploaded depends on the renderer's `ModelShading`. Legacy (the default; the 0.2 behavior): interleaved position/normal/uv/tangent
// vertices, 16- or 32-bit indices, the base color texture of each material (decoded, sRGB, with a full mip chain), a sampler from the
// glTF sampler, and the material's constant factors; the other texture slots are ignored (a material that has one is reported in the
// warnings), so content whose other textures are undecodable still loads. Extended and MetallicRoughness additionally upload the other
// four slots (base color and emissive as sRGB, the rest as linear data, one GPU texture per image and color space, each with the sampler
// its glTF texture names); a slot that cannot be used (bad index, no source image, undecodable image) stays neutral with a warning, while a
// bad base color texture fails the staging in every mode. A normal texture on a primitive without tangents, or one that names a texture
// coordinate set other than 0, is ignored and reported. Alpha-blended materials draw as opaque (warned).
//
// Skinned meshes (docs/rendering/skinning.md). A primitive with JOINTS_0 and WEIGHTS_0 that a skinned node instances is staged as
// SkinnedModelVertex with `skinned` and `joint_limit` set when the renderer was created with skinning; `GpuModel::skin_joint_count` is its skin's joint
// count, and the instance is placed at the identity (glTF ignores a skinned node's own transform). One model has one skin: instances of any other skin are not
// placed and a warning says so. With skinning off the same primitives are staged rigid, in the bind pose (which is what identity skin matrices give), with a
// warning. A joint index at or beyond the skin's joint count, or a skin with more than kMaxSkinJoints joints, fails the staging with a message naming the mesh.
#pragma once

#include "saige/gfx_assets/gltf.hpp"
#include "saige/gfx_assets/image.hpp"
#include "saige/render/model.hpp"
#include "saige/render/scene_renderer.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace saige::gfx {

struct StageSettings {
    ImageLimits image_limits;
    float max_anisotropy = 1.0f;  ///< applied to trilinear samplers; degrades to trilinear on devices without it
};

struct StageReport {
    std::vector<std::string> warnings;  ///< unsupported-but-not-fatal things (deduplicated)
    uint32_t textures = 0;              ///< decoded and uploaded
    uint64_t texture_bytes = 0;         ///< estimated GPU bytes of those, including mips
    uint32_t primitives = 0;
    uint64_t vertex_bytes = 0;
    uint64_t index_bytes = 0;
    uint32_t skinned_primitives = 0;  ///< primitives staged with joints and weights (SkinnedModelVertex)
    double prepare_seconds = 0.0;     ///< CPU preparation (decode, mips, packing), see prepared_model.hpp
    double upload_seconds = 0.0;      ///< GPU object creation and transfers
};

/// Throws std::runtime_error with an actionable message (naming the image, mesh or material) on any failure.
/// `name` prefixes the GPU debug names so the registry's leak report points at the asset.
[[nodiscard]] render::GpuModel stage_model(render::SceneRenderer& renderer, const GltfAsset& asset, std::string_view name,
                                           const StageSettings& settings = {}, StageReport* report = nullptr);

/// glTF sampler (raw OpenGL enum values) to an RHI sampler description.
[[nodiscard]] rhi::SamplerDesc to_sampler_desc(const Sampler& sampler, float max_anisotropy);

} // namespace saige::gfx
