#pragma once

#include "saige/editor_scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace saige::editor {

// Authoritative engine validator domains (implemented by Claude in SAIGE core,
// surfaced by the editor without creating a competing validation system).
enum class EngineValidationDomain : std::uint8_t {
  Collision = 0,
  Navigation = 1,
  Geometry = 2,
  Road = 3,
  Pedestrian = 4,
};

struct EngineValidationFinding {
  EngineValidationDomain domain = EngineValidationDomain::Geometry;
  DiagnosticSeverity severity = DiagnosticSeverity::Warning;
  EntityId node_id = kInvalidEntityId;
  std::string code;
  std::string message;
};

class IWorldValidatorBridge {
 public:
  virtual ~IWorldValidatorBridge() = default;
  [[nodiscard]] virtual std::vector<EngineValidationFinding> validate_scene(
      const EditorScene& scene) const = 0;
};

enum class AssetCategory : std::uint8_t {
  All = 0,
  Building = 1,
  Road = 2,
  Vegetation = 3,
  Prop = 4,
  NavVolume = 5,
  Light = 6,
};

struct AssetCatalogEntry {
  AssetReference ref{};
  std::string display_name;
  AssetCategory category = AssetCategory::Prop;
  EditorNodeKind default_node_kind = EditorNodeKind::Prop;
  bool preview_supported = false;
};

class AssetRegistry {
 public:
  bool register_asset(AssetCatalogEntry entry);
  [[nodiscard]] std::vector<AssetCatalogEntry> search(
      AssetCategory category, const std::string& query) const;
  [[nodiscard]] const AssetCatalogEntry* find_by_uri(
      const std::string& uri) const noexcept;
  [[nodiscard]] std::size_t count() const noexcept { return entries_.size(); }

 private:
  std::vector<AssetCatalogEntry> entries_;
};

enum class ViewportProjection : std::uint8_t {
  Perspective = 0,
  OrthographicTop = 1,
  OrthographicFront = 2,
  OrthographicSide = 3,
};

struct ValidationOverlayFlags {
  bool show_collision = true;
  bool show_navigation = true;
  bool show_geometry = true;
  bool show_roads = true;
  bool show_pedestrian = true;
};

struct ViewportState {
  ViewportProjection projection = ViewportProjection::Perspective;
  Vec3d camera_target{0.0, 0.0, 0.0};
  double orbit_distance_m = 25.0;
  double yaw_deg = 45.0;
  double pitch_deg = -30.0;
  double ortho_width_m = 50.0;
  ValidationOverlayFlags overlays{};

  void pan(double dx, double dy, double dz) noexcept;
  void zoom(double delta_m) noexcept;
  bool focus_on_selection(const EditorScene& scene) noexcept;
};

struct WorldExportResult {
  bool success = false;
  std::string json_payload;
  std::vector<ValidationDiagnostic> blocking_diagnostics;
  std::vector<EngineValidationFinding> engine_findings;
};

class WorldDataExporter {
 public:
  // Exports SAIGE-compatible Z-up JSON world data; refuses export if either
  // authoring diagnostics or authoritative engine validator report Errors.
  [[nodiscard]] static WorldExportResult export_validated_scene(
      const EditorScene& scene,
      const IWorldValidatorBridge* engine_validator = nullptr);
};

}  // namespace saige::editor
