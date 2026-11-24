// Showcase 03: the GPU side of the world. A material library (Poly Haven CC0 textures plus procedural emissive and particle materials), upload of CPU meshes into
// renderer models, procedural tree/chunk/puff models and the licensed prop, car and character models. Everything uploads through the renderer's registry, so the
// resources are released with the renderer.
#pragma once

#include "saige/gfx_assets/gltf.hpp"
#include "saige/render/model.hpp"
#include "saige/render/scene_renderer.hpp"
#include "world.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace saige::horizon {

namespace fs = std::filesystem;

// One CPU mesh to upload: vertices in the layout the model pipelines read, and the index of the material (into the model's material list) it is drawn with.
struct MeshIn {
    std::vector<render::ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t material = 0;
};

// Material list layout shared by every procedural model: the first `surface_count` entries are the world surfaces, then the extras below.
enum Extra : std::uint32_t {
    ExtraDust0 = static_cast<std::uint32_t>(surface_count),  // six blended smoke levels, thin to dense
    ExtraDust1, ExtraDust2, ExtraDust3, ExtraDust4, ExtraDust5,
    ExtraConcrete,   // broken concrete (chunks)
    ExtraRubber,     // dark rubber
    ExtraSpark,      // hot, emissive
    ExtraCount
};
inline constexpr std::uint32_t emissive_levels = 4;  // 0 off (day), 1 dusk, 2 night, 3 bright

class MaterialLibrary {
public:
    MaterialLibrary(render::SceneRenderer& renderer, const fs::path& data_root, std::vector<std::string>& warnings);
    // The material list for an emissive level (windows and lamps glow more at higher levels).
    [[nodiscard]] const std::vector<render::GpuMaterial>& level(std::uint32_t emissive_level) const { return levels_[emissive_level]; }

private:
    std::array<std::vector<render::GpuMaterial>, emissive_levels> levels_;
};

// A model of the given meshes using material list `materials` (their `material` indexes).
[[nodiscard]] render::GpuModel upload_model(render::SceneRenderer& renderer, const std::string& name, const std::vector<MeshIn>& meshes, const std::vector<render::GpuMaterial>& materials);

// A world cell's meshes with tangents generated (one mesh per surface).
[[nodiscard]] std::vector<MeshIn> cell_meshes(const MeshPart* parts, std::size_t count);
[[nodiscard]] std::vector<MeshIn> cell_meshes(const std::vector<MeshPart>& parts);

// Procedural models. Their local frame: +Z up, base at z = 0.
[[nodiscard]] std::vector<MeshIn> tree_pine_meshes(unsigned seed);
[[nodiscard]] std::vector<MeshIn> tree_round_meshes(unsigned seed);
[[nodiscard]] std::vector<MeshIn> chunk_meshes(unsigned seed);   // a jagged concrete lump about 0.4 m across
[[nodiscard]] std::vector<MeshIn> puff_meshes(std::uint32_t material);  // a low-poly sphere of radius 1
[[nodiscard]] std::vector<MeshIn> box_meshes(float sx, float sy, float sz, std::uint32_t material);

// A licensed prop staged as authored (Poly Haven models are in meters with their base at the origin).
[[nodiscard]] render::GpuModel load_prop(render::SceneRenderer& renderer, const fs::path& gltf, std::vector<std::string>& warnings);

// Splits a staged glTF model into the placements whose predicate holds and the rest.
render::GpuModel filter_placements(const render::GpuModel& model, const std::vector<bool>& keep, const render::Mat4& pre = render::Mat4::identity());

// Bounds of the placed meshes of a staged model (engine space), for `keep`-selected placements, from the CPU asset. Returns false if none.
[[nodiscard]] bool placement_bounds(const gfx::GltfAsset& asset, const std::vector<bool>& keep, render::Float3& lo, render::Float3& hi);

} // namespace saige::horizon
