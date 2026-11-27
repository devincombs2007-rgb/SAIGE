#include "saige/editor_semantic.hpp"

#include <cmath>

namespace saige::editor {

namespace {

inline constexpr double kRadToDeg = 57.2957795130823208768;

}  // namespace

PlacementProposal SemanticPlacementEngine::propose_alignment(
    const EditorScene& scene,
    EntityId target_id,
    const SemanticMetadata& target_meta,
    EntityId anchor_id,
    const SemanticMetadata& anchor_meta,
    double ground_z_m,
    double desired_offset_m) {
  PlacementProposal p{};
  p.target_node_id = target_id;
  p.anchor_node_id = anchor_id;

  const EditorNode* target = scene.find_node(target_id);
  const EditorNode* anchor = scene.find_node(anchor_id);
  if (!target || !anchor) {
    p.valid = false;
    p.rationale = "Target or anchor entity not found in scene.";
    return p;
  }
  if (target->locked) {
    p.valid = false;
    p.rationale = "Target entity is locked.";
    return p;
  }

  p.proposed_transform = target->local_transform;

  // Ground contact in Z-up meter convention
  if (target_meta.requires_ground_contact) {
    p.proposed_transform.position.z = ground_z_m;
  }

  const Vec3d& ap = anchor->local_transform.position;
  const Vec3d& tp = target->local_transform.position;

  if (target_meta.role == SemanticRole::Vehicle &&
      anchor_meta.role == SemanticRole::ParkingSpace) {
    p.proposed_transform.position.x = ap.x;
    p.proposed_transform.position.y = ap.y;
    p.proposed_transform.rotation_deg.z = anchor->local_transform.rotation_deg.z;
    p.rationale = "Aligned vehicle center and yaw to parking space.";
    return p;
  }

  // Face anchor (chair -> table, sofa -> TV, road sign -> lane, streetlight -> sidewalk)
  double dx = ap.x - tp.x;
  double dy = ap.y - tp.y;
  const double dist = std::hypot(dx, dy);
  if (dist > 1e-4 && desired_offset_m > 0.0 &&
      (target_meta.role == SemanticRole::Chair ||
       target_meta.role == SemanticRole::Sofa)) {
    const double nx = dx / dist;
    const double ny = dy / dist;
    p.proposed_transform.position.x = ap.x - nx * desired_offset_m;
    p.proposed_transform.position.y = ap.y - ny * desired_offset_m;
    dx = ap.x - p.proposed_transform.position.x;
    dy = ap.y - p.proposed_transform.position.y;
  }
  if (std::hypot(dx, dy) > 1e-6) {
    // +Y is local forward (yaw = 0 -> +Y, yaw = -90 -> +X)
    const double yaw_deg = std::atan2(-dx, dy) * kRadToDeg;
    p.proposed_transform.rotation_deg.z = yaw_deg;
  }
  p.rationale = "Aligned entity yaw toward semantic anchor and grounded Z.";
  return p;
}

std::vector<ValidationDiagnostic> SemanticPlacementEngine::validate_clearance(
    const EditorScene& scene,
    EntityId node_id,
    const SemanticMetadata& meta) {
  std::vector<ValidationDiagnostic> diags;
  const EditorNode* node = scene.find_node(node_id);
  if (!node) return diags;

  if (meta.requires_ground_contact && node->local_transform.position.z > 0.05) {
    diags.push_back({DiagnosticSeverity::Warning, node_id, "FLOATING_OBJECT",
                     "Grounded entity floats above Z=0 reference plane."});
  }

  const auto chunk_ids = scene.query_chunk(node->chunk);
  for (EntityId other_id : chunk_ids) {
    if (other_id == node_id) continue;
    const EditorNode* other = scene.find_node(other_id);
    if (!other) continue;
    const double d = std::hypot(
        node->local_transform.position.x - other->local_transform.position.x,
        node->local_transform.position.y - other->local_transform.position.y);
    if (d < meta.clearance_radius_m) {
      diags.push_back({DiagnosticSeverity::Warning, node_id, "CLEARANCE_VIOLATION",
                       "Entity overlaps clearance radius of node '" + other->name + "'."});
    }
  }
  return diags;
}

ApplySemanticPlacementCommand::ApplySemanticPlacementCommand(
    PlacementProposal proposal)
    : proposal_(std::move(proposal)) {}

bool ApplySemanticPlacementCommand::execute(EditorScene& scene) {
  if (!proposal_.valid || !proposal_.proposed_transform.is_valid()) return false;
  EditorNode* n = scene.find_node(proposal_.target_node_id);
  if (!n || n->locked) return false;
  old_transform_ = n->local_transform;
  return scene.set_transform(proposal_.target_node_id, proposal_.proposed_transform);
}

void ApplySemanticPlacementCommand::undo(EditorScene& scene) {
  (void)scene.set_transform(proposal_.target_node_id, old_transform_);
}

}  // namespace saige::editor
