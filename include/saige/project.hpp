#pragma once

#include "saige/asset_id.hpp"
#include "saige/scene_hierarchy.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace saige {

struct ProjectAsset {
    AssetId id;
    std::string relative_path;
};

struct ProjectNode {
    AssetId id; // Node IDs and asset IDs use separate namespaces.
    std::optional<AssetId> parent;
    AssetId mesh; // Zero selects the built-in mesh.
    LocalTransform local;
};

struct ProjectCamera {
    Vec3 position{};
    Quaternion rotation{};
    float fov_y_radians{1.04719755F};
    float near_plane{0.1F};
};

struct Project {
    std::vector<ProjectAsset> assets;
    std::vector<ProjectNode> nodes;
    ProjectCamera camera;
};

// Returns a fully validated project. It never mutates the caller's active scene.
[[nodiscard]] Project load_project(const std::filesystem::path& path);
void save_project(const Project& project, const std::filesystem::path& path);
[[nodiscard]] std::vector<WorldMatrix> resolve_project_matrices(const Project& project);

} // namespace saige
