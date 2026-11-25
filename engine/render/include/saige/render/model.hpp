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

class SkinPalette;

/// Interleaved vertex: position, normal, uv, tangent (48 bytes), the layout the model pipelines declare. The tangent is glTF's:
/// xyz is the direction of increasing u, w is the bitangent's handedness (+1 or -1). w = 0 means "no tangent": normal mapping
/// is skipped for the vertex's triangle.
struct ModelVertex {
    Float3 position;
    Float3 normal;
    float uv[2] = {0.0f, 0.0f};
    float tangent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(ModelVertex) == 48, "ModelVertex must match the pipeline's vertex layout");

/// A skinned vertex: a ModelVertex followed by the four joints that influence it and their weights (72 bytes), the layout of the skinned
/// pipelines. Joints index the skin's joint list; weights are non-negative and sum to 1 (a vertex with fewer than four influences carries
/// zero weights for the rest). See docs/rendering/skinning.md.
struct SkinnedModelVertex {
    ModelVertex base;
    uint16_t joints[4] = {0, 0, 0, 0};
    float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(SkinnedModelVertex) == 72, "SkinnedModelVertex must match the skinned pipelines' vertex layout");

/// The most joints one skin palette holds: 256 affine matrices of 48 bytes fit one 16 KiB uniform binding, the portable minimum.
inline constexpr uint32_t kMaxSkinJoints = 256;

/// The axis-aligned box, in the skinned mesh's bind space, of the vertices a joint influences (weight above zero); `used` is false for a joint no vertex uses.
struct JointBounds {
    Float3 min{};
    Float3 max{};
    bool used = false;
};

struct GpuPrimitive {
    BufferRef vertices;
    BufferRef indices;
    uint32_t index_count = 0;
    rhi::IndexType index_type = rhi::IndexType::Uint32;
    uint32_t material = 0;  ///< index into GpuModel::materials (always valid)
    /// Bounding sphere in the model's local space. Blended primitives are sorted by the view-space depth of its center; the
    /// default (the origin, radius 0) sorts a primitive by its model's origin. The importer's staging fills it from the positions.
    Float3 bounds_center{};
    float bounds_radius = 0.0f;
    /// True when `vertices` holds SkinnedModelVertex instead of ModelVertex. Such a primitive needs a renderer created with
    /// `ModelRenderSettings::skinning` and is drawn through the draw's SkinPalette (the bind pose when the draw has none).
    bool skinned = false;
    /// For a skinned primitive: one more than the largest joint index any vertex references. A draw whose palette has fewer joints skips it
    /// (counted in ModelFrameStats::skinned_skipped) instead of reading past the palette.
    uint32_t joint_limit = 0;
    /// For a skinned primitive staged with per-pose bounds: one box per joint (size `joint_limit`), in bind space. A posed vertex is a convex combination of the points
    /// `skin_matrix[j] * bind_position` of the joints that influence it, so the union of each joint's box transformed by that joint's skin matrix bounds the posed primitive,
    /// whatever the pose (ModelRenderer culls with it). Empty: no bound is known (hand-built models, weights that do not sum to one) and the primitive is never culled.
    std::vector<JointBounds> joint_bounds;
};

struct GpuMaterial {
    BindGroupRef group;       ///< set 0 of the model pipelines; keeps the textures, sampler and parameters alive
    bool double_sided = false;
    bool blend = false;       ///< drawn source-over in the sorted phase after opaque draws (set by create_material)
    bool casts_sun_shadow = true;  ///< from MaterialData::casts_sun_shadow: false for authored BLEND materials
    bool shadow_alpha_test = false;  ///< casts through the alpha-tested shadow variant: a cutoff above 0 (authored MASK)
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
    /// Joints of the skin the model's skinned primitives are bound to (0: no skinned primitives). A draw of this model supplies a SkinPalette created with
    /// at least this many joints (ModelRenderer::create_skin_palette); one model has one skin, so one palette per drawn instance.
    uint32_t skin_joint_count = 0;

    [[nodiscard]] uint64_t triangle_count() const noexcept;
};

/// One draw of a model: the model and its world matrix (engine world space, column-major, the authoritative affine).
struct ModelDraw {
    const GpuModel* model = nullptr;
    Mat4 world = Mat4::identity();
    /// The pose of this instance's skinned primitives (null: the bind pose). Rigid primitives ignore it. One palette per animated instance;
    /// several draws of the same model with different palettes are several animated instances.
    const SkinPalette* palette = nullptr;
};

} // namespace saige::render
