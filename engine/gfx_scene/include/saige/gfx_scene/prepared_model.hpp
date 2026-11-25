// SAIGE gfx_scene — a glTF asset prepared on the CPU, and its upload.
//
// `stage_model` does two kinds of work: CPU work that needs no device (decode the images, build the mip chains, pack the vertices and indices, compute the bounds, resolve
// the materials and placements, collect the warnings) and GPU work (create the textures, buffers, samplers and materials, which waits on the device's transfers). This header
// separates them so a host can run the first on a worker and the second at a point it chooses, and measure them apart:
//
//   PreparedModel prepared = prepare_model(asset, "name", capabilities_of(renderer));   // CPU only: no renderer, no device, any thread
//   GpuModel model = upload_model(renderer, prepared);                                    // GPU: the render thread; the payload is not consumed
//
// `stage_model(renderer, asset, ...)` is exactly `upload_model(renderer, prepare_model(asset, ..., capabilities_of(renderer)))`.
//
// The payload is a value: immutable once built, owned by whoever holds it, and **reusable**: uploading the same payload twice gives two independent GpuModels (for two
// renderers, or a re-upload after a device loss), and a payload built for some capabilities is refused (std::invalid_argument, before anything is created) by a renderer whose
// capabilities differ, because the payload's contents depend on them (which texture slots are decoded, whether skinned vertices are packed, how blended materials are flagged).
// Preparing for several material modes means preparing once per mode; the payload does not guess.
//
// Failure semantics match stage_model's: `prepare_model` throws std::runtime_error naming the asset, mesh, image or material for anything wrong with the asset (an undecodable
// base color image, a bad index, a joint beyond the skin) and has no effect; `upload_model` is all-or-nothing on the device (the references it created are released as the stack
// unwinds) and never touches anything the renderer was already showing. Cancellation and budgets are the host's: a prepared payload that is never uploaded costs only its CPU
// memory (`PreparedModel::cpu_bytes`), and an upload that fails or is dropped has changed nothing. Design and the asynchronous transfer ticket this prepares for:
// docs/rendering/prepared-model.md.
#pragma once

#include "saige/gfx_scene/stage_model.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace saige::gfx {

/// The renderer configuration the payload's contents depend on.
struct StageCapabilities {
    render::ModelShading shading = render::ModelShading::Legacy;  ///< which texture slots are decoded and uploaded
    bool skinning = false;                                        ///< skinned primitives are packed as SkinnedModelVertex with joints and weights
    bool alpha_blending = false;                                  ///< BLEND materials are flagged for the sorted phase (only with `blending_supported`)
    bool blending_supported = false;                              ///< the scene's color format can blend
    friend bool operator==(const StageCapabilities&, const StageCapabilities&) noexcept = default;
};

/// What `renderer` is configured with (creates its model renderer if it does not exist yet).
[[nodiscard]] StageCapabilities capabilities_of(render::SceneRenderer& renderer);

struct PreparedImage {
    std::string label;      ///< for error messages and GPU debug names
    bool srgb = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t levels = 0;
    std::vector<uint8_t> data;  ///< the whole mip chain, tightly packed, level 0 first
};

/// The GPU texture description a prepared image is uploaded with (its color space decides the format; `debug_name` is left unset).
[[nodiscard]] rhi::TextureDesc texture_desc_of(const PreparedImage& image);

struct PreparedSlot {
    int32_t image = -1;            ///< index into PreparedModel::images, or -1 for the neutral texture
    rhi::SamplerDesc sampler{};
};

struct PreparedMaterial {
    render::MaterialData data;
    PreparedSlot slots[render::kMaterialSlotCount];
    std::string label;
};

struct PreparedPrimitive {
    std::string where;                  ///< "name/mesh[i] 'x' primitive j", for error messages
    std::vector<std::byte> vertices;    ///< ModelVertex or SkinnedModelVertex, packed
    std::vector<std::byte> indices;     ///< uint16 or uint32
    rhi::IndexType index_type = rhi::IndexType::Uint32;
    uint32_t index_count = 0;
    bool skinned = false;
    uint32_t joint_limit = 0;
    std::vector<render::JointBounds> joint_bounds;  ///< per-joint bind-space boxes for conservative per-pose culling; empty: never culled (weights not convex)
    render::Float3 bounds_center{};
    float bounds_radius = 0.0f;
    uint32_t material = 0;              ///< into PreparedModel::materials (the last one is the default material)
};

struct PreparedModel {
    std::string name;
    StageCapabilities capabilities;
    std::vector<PreparedImage> images;
    std::vector<PreparedMaterial> materials;  ///< the asset's, then the default material
    std::vector<std::vector<PreparedPrimitive>> meshes;
    std::vector<render::GpuPlacement> placements;
    uint32_t skin_joint_count = 0;
    StageReport report;               ///< warnings, and the primitive/vertex/index/skinned counts known without a device
    double prepare_seconds = 0.0;     ///< wall time of prepare_model (decode, mips and packing included)
    double decode_seconds = 0.0;      ///< of which: image decoding and mip generation

    /// Host memory the payload holds (images, vertices, indices).
    [[nodiscard]] uint64_t cpu_bytes() const noexcept;
};

/// CPU only. Throws std::runtime_error (naming the asset, mesh, image or material) for anything wrong with the asset, with no effect.
[[nodiscard]] PreparedModel prepare_model(const GltfAsset& asset, std::string_view name, const StageCapabilities& capabilities, const StageSettings& settings = {});

/// Creates the GPU objects of `prepared` on `renderer`. Throws std::invalid_argument, before creating anything, if the renderer's capabilities are not the payload's, and
/// std::runtime_error for a device failure (everything created is released). `report` receives the payload's report plus the texture counts and GPU bytes; `upload_seconds`
/// the wall time of the upload (the device's transfer waits included).
[[nodiscard]] render::GpuModel upload_model(render::SceneRenderer& renderer, const PreparedModel& prepared, StageReport* report = nullptr, double* upload_seconds = nullptr);

} // namespace saige::gfx
