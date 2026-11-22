// SAIGE gfx_scene — turns an imported glTF asset (CPU data from gfx_assets) into a GpuModel the renderer can draw.
//
// Staging is all-or-nothing. Every GPU resource is created through the renderer's registry and held by shared references;
// if any step fails (an undecodable image, a device error) the function throws and the references it created are
// released as the stack unwinds, so the caller keeps whatever scene it was already showing. A successful result is a
// complete candidate: activating it is moving it into place, and dropping the previous one retires its resources on the
// device's completion timeline.
//
// What is uploaded: interleaved position/normal/uv/tangent vertices, 16- or 32-bit indices, each material's five textures
// (base color and emissive decoded as sRGB, metallic-roughness, normal and occlusion as linear data, each with a full mip
// chain and the sampler its glTF texture names) and the material's constant factors. All five are shaded (the last three
// in metallic-roughness mode). A normal texture on a primitive without tangents is ignored and reported. Alpha-blended
// materials draw as opaque (warned).
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
};

/// Throws std::runtime_error with an actionable message (naming the image, mesh or material) on any failure.
/// `name` prefixes the GPU debug names so the registry's leak report points at the asset.
[[nodiscard]] render::GpuModel stage_model(render::SceneRenderer& renderer, const GltfAsset& asset, std::string_view name,
                                           const StageSettings& settings = {}, StageReport* report = nullptr);

/// glTF sampler (raw OpenGL enum values) to an RHI sampler description.
[[nodiscard]] rhi::SamplerDesc to_sampler_desc(const Sampler& sampler, float max_anisotropy);

} // namespace saige::gfx
