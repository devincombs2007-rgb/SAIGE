// SAIGE gfx_assets — glTF 2.0 import to CPU mesh/material data, in engine space.
//
// What is supported (and what is rejected) is defined by docs/rendering/gltf-subset.md. Everything returned is in the
// engine's world convention (right-handed, +Z up, +X forward, +Y left, meters): the importer converts from glTF's
// Y-up at the boundary with a proper rotation, so triangle winding is preserved (counter-clockwise stays front-facing).
//
// The loader treats its input as hostile: every offset is bounds-checked with overflow-safe arithmetic, declared sizes
// are validated against limits before anything large is allocated, resource URIs follow a strict relative-path policy,
// and malformed input produces a descriptive error, never a crash. Exceptions are used internally and never escape.
#pragma once

#include "saige/render/math.hpp"
#include "saige/render/transform.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace saige::gfx {

struct Aabb {
    render::Float3 min{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                       std::numeric_limits<float>::max()};
    render::Float3 max{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                       std::numeric_limits<float>::lowest()};

    [[nodiscard]] bool valid() const noexcept { return min.x <= max.x && min.y <= max.y && min.z <= max.z; }
    void expand(render::Float3 p) noexcept;
    void expand(const Aabb& other) noexcept;
};

struct TextureRef {
    int32_t texture = -1;     ///< index into GltfAsset::textures, -1 = none
    uint32_t tex_coord = 0;   ///< the renderer only samples TEXCOORD_0 (a non-zero value is warned about)
    [[nodiscard]] bool valid() const noexcept { return texture >= 0; }
};

enum class AlphaMode : uint8_t { Opaque, Mask, Blend };

/// glTF metallic-roughness material with the specification's defaults.
struct Material {
    std::string name;
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 1.0f;
    float roughness = 1.0f;
    float emissive[3] = {0.0f, 0.0f, 0.0f};  ///< already multiplied by KHR_materials_emissive_strength
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    float alpha_cutoff = 0.5f;
    AlphaMode alpha_mode = AlphaMode::Opaque;
    bool double_sided = false;
    TextureRef base_color_texture;
    TextureRef metallic_roughness_texture;  ///< G = roughness, B = metallic
    TextureRef normal_texture;
    TextureRef occlusion_texture;           ///< R
    TextureRef emissive_texture;
};

/// Raw glTF/OpenGL enum values are kept as given (0 = unspecified).
struct Sampler {
    uint32_t mag_filter = 0;
    uint32_t min_filter = 0;
    uint32_t wrap_s = 10497;  ///< REPEAT
    uint32_t wrap_t = 10497;
};

/// An image kept *encoded* (PNG/JPEG bytes). Decoding is a separate step.
struct Image {
    std::string name;
    std::string path;       ///< normalized relative path when loaded from a file, empty when embedded
    std::string mime_type;  ///< may be empty if neither the file nor the extension says
    std::vector<uint8_t> bytes;
};

struct Texture {
    int32_t image = -1;
    int32_t sampler = -1;  ///< -1 = default sampler
};

struct Primitive {
    std::vector<float> positions;  ///< xyz per vertex, engine space
    std::vector<float> normals;    ///< xyz per vertex, unit length (generated flat normals if the file had none)
    std::vector<float> tangents;   ///< xyzw per vertex, empty if absent
    std::vector<float> uv0;        ///< uv per vertex (origin top-left, like Vulkan), empty if absent
    std::vector<float> colors;     ///< rgba per vertex, empty if absent
    std::vector<uint32_t> indices; ///< triangle list, counter-clockwise front faces
    int32_t material = -1;
    bool normals_generated = false;
    Aabb bounds;

    [[nodiscard]] uint32_t vertex_count() const noexcept { return static_cast<uint32_t>(positions.size() / 3); }
};

struct Mesh {
    std::string name;
    std::vector<Primitive> primitives;
    Aabb bounds;
};

/// One placement of a mesh in the scene: the node hierarchy is flattened into a world matrix in engine space.
struct MeshInstance {
    int32_t mesh = -1;
    int32_t node = -1;
    render::Mat4 world;  ///< engine space; places the (already converted) mesh vertices
    std::string name;
};

struct GltfAsset {
    std::vector<Mesh> meshes;
    std::vector<Material> materials;
    std::vector<Texture> textures;
    std::vector<Image> images;
    std::vector<Sampler> samplers;
    std::vector<MeshInstance> instances;
    Aabb bounds;                        ///< of all instances in engine space
    std::vector<std::string> warnings;  ///< things ignored or unsupported but not fatal (deduplicated)
};

struct GltfLimits {
    std::size_t max_file_bytes = std::size_t{256} << 20;
    std::size_t max_buffer_bytes = std::size_t{256} << 20;
    std::size_t max_total_buffer_bytes = std::size_t{512} << 20;
    std::size_t max_accessor_count = std::size_t{1} << 26;
    std::size_t max_vertices_per_primitive = std::size_t{1} << 23;
    std::size_t max_instances = std::size_t{1} << 20;
    std::size_t max_node_depth = 256;
};

/// Reads an external resource given a *validated, normalized, relative* path. Returns nullopt if it can't be read.
using ResourceReader = std::function<std::optional<std::vector<uint8_t>>(const std::string& relative_path)>;

struct GltfLoadResult {
    std::optional<GltfAsset> asset;
    std::string error;  ///< set when asset is empty
    [[nodiscard]] bool ok() const noexcept { return asset.has_value(); }
};

/// Loads a `.gltf` (JSON text) or `.glb` (binary container) held in memory. External resources go through `reader`;
/// pass an empty function to forbid them (only data URIs and the GLB BIN chunk are then usable).
[[nodiscard]] GltfLoadResult load_gltf(std::span<const uint8_t> bytes, const ResourceReader& reader,
                                       const GltfLimits& limits = {});

/// Reads a file from disk. External resources are confined to the file's own directory: each validated relative path is
/// resolved against it and refused if the result (after resolving symlinks) is outside it.
[[nodiscard]] GltfLoadResult load_gltf_file(const std::filesystem::path& path, const GltfLimits& limits = {});

/// glTF (+Y up, front +Z) to engine (+Z up, front +X, +Y left): (x, y, z) -> (z, x, y). A proper rotation.
[[nodiscard]] constexpr render::Float3 gltf_to_engine(render::Float3 v) noexcept
{
    return {v.z, v.x, v.y};
}

} // namespace saige::gfx
