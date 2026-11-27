#pragma once

#include "saige/editor_scene.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace saige::editor {

enum class LibraryAssetCategory : std::uint8_t {
  Vehicle = 0,
  Building = 1,
  Interior = 2,
  Environment = 3,
  Character = 4,
  Material = 5,
  Light = 6,
};

struct Aabb3d {
  Vec3d min{};
  Vec3d max{};

  [[nodiscard]] bool is_valid() const noexcept {
    return min.x <= max.x && min.y <= max.y && min.z <= max.z;
  }
  [[nodiscard]] bool contains_with_margin(const Vec3d& p,
                                          double margin_m) const noexcept {
    return p.x >= (min.x - margin_m) && p.x <= (max.x + margin_m) &&
           p.y >= (min.y - margin_m) && p.y <= (max.y + margin_m) &&
           p.z >= (min.z - margin_m) && p.z <= (max.z + margin_m);
  }
};

struct VehicleAttachmentPoint {
  std::string name;  // e.g., "hazard_light_fl", "wheel_fl", "driver_seat"
  Vec3d local_pos_m{};
  double tolerance_m = 0.25;
};

struct VehicleAssetMetadata {
  Aabb3d body_bounds_m{{-1.0, -2.3, 0.0}, {1.0, 2.3, 1.5}};
  double wheel_radius_m = 0.34;
  std::vector<VehicleAttachmentPoint> wheels;
  std::vector<VehicleAttachmentPoint> lights;  // headlights, brake, hazard, turn
  std::vector<VehicleAttachmentPoint> seats;
};

struct AssetMetadata {
  std::uint64_t asset_id = 0;
  std::string uri;
  std::string display_name;
  LibraryAssetCategory category = LibraryAssetCategory::Environment;
  std::vector<std::string> tags;
  std::vector<std::string> collections;
  bool favorite = false;

  // Provenance & Licensing
  std::string source_url;
  std::string author;
  std::string license_spdx;  // e.g., "CC0-1.0", "CC-BY-4.0"
  bool redistribution_allowed = false;

  // Versioning & Dependencies
  std::uint32_t version = 1;
  std::uint64_t content_hash = 0;
  std::vector<std::string> dependencies;
  std::uint64_t usage_count = 0;

  // Geometry / Quality metadata
  double unit_scale_m = 1.0;
  bool z_up_right_handed = true;
  std::uint64_t triangle_count = 0;
  bool has_normals = true;
  bool has_uvs = true;
  bool has_collision_mesh = true;
  std::uint32_t lod_count = 1;

  std::optional<VehicleAssetMetadata> vehicle_meta;
};

struct AssetValidationIssue {
  DiagnosticSeverity severity = DiagnosticSeverity::Warning;
  std::uint64_t asset_id = 0;
  std::string uri;
  std::string code;
  std::string message;
  std::string suggestion;
};

struct AssetValidationReport {
  bool passed = true;
  std::size_t error_count = 0;
  std::size_t warning_count = 0;
  std::vector<AssetValidationIssue> issues;
};

class AssetLibrary {
 public:
  bool register_asset(AssetMetadata meta);
  bool remove_asset(std::uint64_t asset_id);
  bool set_favorite(std::uint64_t asset_id, bool fav);
  bool increment_usage(const std::string& uri);

  [[nodiscard]] const AssetMetadata* find_by_id(
      std::uint64_t asset_id) const noexcept;
  [[nodiscard]] const AssetMetadata* find_by_uri(
      const std::string& uri) const noexcept;
  [[nodiscard]] std::vector<const AssetMetadata*> search(
      std::optional<LibraryAssetCategory> category,
      const std::string& query,
      const std::string& required_tag = "",
      bool favorites_only = false) const;

  [[nodiscard]] AssetValidationReport validate_asset(
      std::uint64_t asset_id) const;
  [[nodiscard]] AssetValidationReport validate_all() const;

  [[nodiscard]] std::size_t size() const noexcept { return assets_.size(); }

 private:
  std::vector<AssetMetadata> assets_;
  std::unordered_map<std::uint64_t, std::size_t> id_to_idx_;
  std::unordered_map<std::string, std::size_t> uri_to_idx_;
  std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> hash_to_ids_;
};

}  // namespace saige::editor
