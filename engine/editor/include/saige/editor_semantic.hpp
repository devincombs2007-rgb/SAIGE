#pragma once

#include "saige/editor_commands.hpp"
#include "saige/editor_scene.hpp"

#include <optional>
#include <string>
#include <vector>

namespace saige::editor {

enum class SemanticRole : std::uint8_t {
  Generic = 0,
  Chair = 1,
  Table = 2,
  Sofa = 3,
  Vehicle = 4,
  ParkingSpace = 5,
  Streetlight = 6,
  RoadSign = 7,
  Sidewalk = 8,
  RoadLane = 9,
};

struct SemanticMetadata {
  SemanticRole role = SemanticRole::Generic;
  Vec3d local_front_dir{1.0, 0.0, 0.0};  // +X forward: the engine convention (SAIGE, 2026-11-27; Build 012 assumed +Y). An asset authored with
                                         // another front states it here, and placement turns that axis toward the anchor.
  Vec3d local_up_dir{0.0, 0.0, 1.0};     // +Z up
  bool requires_ground_contact = true;
  double clearance_radius_m = 0.5;
  double min_walkway_clearance_m = 1.0;  // the free gap a pedestrian needs beside this object (Build 012 stored it and never used it)
};

/// Semantic metadata is persisted with the node as versioned properties (`semantic.v1.*`), so the existing project format stores it without a
/// format change. `store` validates first and returns false (changing nothing) for non-finite values, a zero direction or negative distances;
/// `load` returns nothing for a node without the properties and refuses ones that fail the same validation.
[[nodiscard]] bool validate_semantic_metadata(const SemanticMetadata& meta) noexcept;
bool store_semantic_metadata(EditorScene& scene, EntityId node_id, const SemanticMetadata& meta);
[[nodiscard]] std::optional<SemanticMetadata> load_semantic_metadata(const EditorNode& node);

struct PlacementProposal {
  EntityId target_node_id = kInvalidEntityId;
  EntityId anchor_node_id = kInvalidEntityId;
  Transform3d proposed_transform{};
  bool valid = true;
  std::string rationale;
};

class SemanticPlacementEngine {
 public:
  // Computes a yaw-aligned + ground-contacted transform for `target` relative
  // to `anchor` (e.g., chair facing table, vehicle aligned to parking space,
  // streetlight facing sidewalk, road sign facing approaching lane).
  [[nodiscard]] static PlacementProposal propose_alignment(
      const EditorScene& scene,
      EntityId target_id,
      const SemanticMetadata& target_meta,
      EntityId anchor_id,
      const SemanticMetadata& anchor_meta,
      double ground_z_m = 0.0,
      double desired_offset_m = 1.2);

  // Validates walkway/clearance against nearby nodes in the same chunk
  /// In world space, across chunk boundaries (SAIGE, 2026-11-27: Build 012 compared local positions within the node's own chunk only, so objects
  /// 0.2 m apart across x = 64 m were never compared). Nodes in the same hierarchy (ancestors and descendants) are composed deliberately and are not
  /// compared. FLOATING_OBJECT is measured from `ground_z_m`; WALKWAY_NARROW (Info) reports a gap below `min_walkway_clearance_m`.
  [[nodiscard]] static std::vector<ValidationDiagnostic> validate_clearance(
      const EditorScene& scene,
      EntityId node_id,
      const SemanticMetadata& meta,
      double ground_z_m = 0.0);
};

// Reversible command applying a validated PlacementProposal via CommandHistory.
class ApplySemanticPlacementCommand final : public IEditorCommand {
 public:
  explicit ApplySemanticPlacementCommand(PlacementProposal proposal);
  [[nodiscard]] const char* label() const noexcept override {
    return "ApplySemanticPlacement";
  }
  bool execute(EditorScene& scene) override;
  void undo(EditorScene& scene) override;

 private:
  PlacementProposal proposal_;
  Transform3d old_transform_{};
};

}  // namespace saige::editor
