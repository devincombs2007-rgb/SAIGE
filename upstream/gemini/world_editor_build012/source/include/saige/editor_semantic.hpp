#pragma once

#include "saige/editor_commands.hpp"
#include "saige/editor_scene.hpp"

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
  Vec3d local_front_dir{0.0, 1.0, 0.0};  // +Y forward in Z-up
  Vec3d local_up_dir{0.0, 0.0, 1.0};     // +Z up
  bool requires_ground_contact = true;
  double clearance_radius_m = 0.5;
  double min_walkway_clearance_m = 1.0;
};

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
  [[nodiscard]] static std::vector<ValidationDiagnostic> validate_clearance(
      const EditorScene& scene,
      EntityId node_id,
      const SemanticMetadata& meta);
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
