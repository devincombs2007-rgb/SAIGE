// SAIGE Render — imported models on the GPU: the data a ModelRenderer draws, with no dependency on any file format.
//
// A GpuModel holds shared references (gpu_resources.hpp) to its vertex and index buffers and to one bind group per
// material, so everything a draw needs stays alive for as long as the model does, and goes away with it. The glTF importer
// fills these in (engine/gfx_scene); tests can build them by hand.
#pragma once

#include "saige/render/gpu_resources.hpp"
#include "saige/render/math.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace saige::render {

/// Interleaved vertex: position, normal, uv (32 bytes), the layout the model pipelines declare.
struct ModelVertex {
    Float3 position;
    Float3 normal;
    float uv[2] = {0.0f, 0.0f};
};
static_assert(sizeof(ModelVertex) == 32, "ModelVertex must match the pipeline's vertex layout");

struct GpuPrimitive {
    BufferRef vertices;
    BufferRef indices;
    uint32_t index_count = 0;
    rhi::IndexType index_type = rhi::IndexType::Uint32;
    uint32_t material = 0;  ///< index into GpuModel::materials (always valid)
};

struct GpuMaterial {
    BindGroupRef group;       ///< set 0 of the model pipelines; keeps the textures, sampler and parameters alive
    bool double_sided = false;
};

/// One placement of a mesh within the model (a glTF node, flattened), in the model's own space.
struct GpuPlacement {
    uint32_t mesh = 0;
    Mat4 local = Mat4::identity();
};

struct GpuModel {
    std::string name;
    std::vector<GpuMaterial> materials;
    std::vector<std::vector<GpuPrimitive>> meshes;
    std::vector<GpuPlacement> placements;

    [[nodiscard]] uint64_t triangle_count() const noexcept;
};

/// One draw of a model: the model and its world matrix (engine world space, column-major, the authoritative affine).
struct ModelDraw {
    const GpuModel* model = nullptr;
    Mat4 world = Mat4::identity();
};

} // namespace saige::render
