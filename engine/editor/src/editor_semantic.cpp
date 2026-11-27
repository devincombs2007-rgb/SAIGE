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
    // The authored front axis (local, in the XY plane) is turned toward the anchor: yaw = heading of the anchor minus heading of the front axis.
    // With the engine's +X front, a target looking at an anchor along +X gets yaw 0 (Build 012 hard-coded +Y and gave -90 there).
    const double front_heading = std::atan2(target_meta.local_front_dir.y, target_meta.local_front_dir.x);
    const double yaw_deg = (std::atan2(dy, dx) - front_heading) * kRadToDeg;
    p.proposed_transform.rotation_deg.z = yaw_deg;
  }
  p.rationale = "Aligned entity yaw toward semantic anchor and grounded Z.";
  return p;
}

std::vector<ValidationDiagnostic> SemanticPlacementEngine::validate_clearance(
    const EditorScene& scene,
    EntityId node_id,
    const SemanticMetadata& meta,
    double ground_z_m) {
  std::vector<ValidationDiagnostic> diags;
  const EditorNode* node = scene.find_node(node_id);
  if (!node) return diags;
  Vec3d here{};
  if (!scene.world_position(node_id, here)) {
    diags.push_back({DiagnosticSeverity::Error, node_id, "INVALID_WORLD_TRANSFORM",
                     "The node's world position could not be resolved (a parent cycle, a missing parent or a non-finite transform)."});
    return diags;
  }
  if (!validate_semantic_metadata(meta) || !std::isfinite(ground_z_m)) {
    diags.push_back({DiagnosticSeverity::Error, node_id, "INVALID_SEMANTIC_METADATA",
                     "Semantic metadata or the ground height is not finite or not in range."});
    return diags;
  }

  if (meta.requires_ground_contact && here.z - ground_z_m > 0.05) {
    diags.push_back({DiagnosticSeverity::Warning, node_id, "FLOATING_OBJECT",
                     "Grounded entity floats above the ground height."});
  }

  for (const EditorNode& other : scene.nodes()) {
    if (other.id == node_id || scene.is_ancestor_of(other.id, node_id) || scene.is_ancestor_of(node_id, other.id)) continue;
    Vec3d there{};
    if (!scene.world_position(other.id, there)) continue;
    const double d = std::hypot(here.x - there.x, here.y - there.y);
    if (d < meta.clearance_radius_m) {
      diags.push_back({DiagnosticSeverity::Warning, node_id, "CLEARANCE_VIOLATION",
                       "Entity overlaps clearance radius of node '" + other.name + "'."});
    } else if (meta.min_walkway_clearance_m > 0.0 && d < meta.clearance_radius_m + meta.min_walkway_clearance_m) {
      diags.push_back({DiagnosticSeverity::Info, node_id, "WALKWAY_NARROW",
                       "The gap to node '" + other.name + "' is narrower than the walkway clearance."});
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

bool validate_semantic_metadata(const SemanticMetadata& m) noexcept {
  const auto finite3 = [](const Vec3d& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
  const auto nonzero = [](const Vec3d& v) { return std::hypot(v.x, v.y, v.z) > 1e-6; };
  return static_cast<std::uint8_t>(m.role) <= static_cast<std::uint8_t>(SemanticRole::RoadLane) && finite3(m.local_front_dir) &&
         finite3(m.local_up_dir) && nonzero(m.local_front_dir) && nonzero(m.local_up_dir) &&
         std::hypot(m.local_front_dir.x, m.local_front_dir.y) > 1e-6 &&  // the front must have a horizontal component to define a yaw
         std::isfinite(m.clearance_radius_m) && m.clearance_radius_m >= 0.0 && std::isfinite(m.min_walkway_clearance_m) &&
         m.min_walkway_clearance_m >= 0.0;
}

namespace {
PropertyValue prop_int(std::int64_t v) { PropertyValue p; p.type = PropertyType::Int64; p.int_val = v; return p; }
PropertyValue prop_double(double v) { PropertyValue p; p.type = PropertyType::Double; p.double_val = v; return p; }
PropertyValue prop_bool(bool v) { PropertyValue p; p.type = PropertyType::Bool; p.bool_val = v; return p; }
PropertyValue prop_vec3(const Vec3d& v) { PropertyValue p; p.type = PropertyType::Vec3; p.vec3_val = v; return p; }
const PropertyValue* find_prop(const EditorNode& n, const char* key, PropertyType type) {
  for (const auto& e : n.properties) {
    if (e.key == key) return e.value.type == type ? &e.value : nullptr;
  }
  return nullptr;
}
}  // namespace

bool store_semantic_metadata(EditorScene& scene, EntityId node_id, const SemanticMetadata& meta) {
  if (!validate_semantic_metadata(meta) || scene.find_node(node_id) == nullptr) return false;
  return scene.set_property(node_id, "semantic.v1.version", prop_int(1)) &&
         scene.set_property(node_id, "semantic.v1.role", prop_int(static_cast<std::int64_t>(meta.role))) &&
         scene.set_property(node_id, "semantic.v1.front", prop_vec3(meta.local_front_dir)) &&
         scene.set_property(node_id, "semantic.v1.up", prop_vec3(meta.local_up_dir)) &&
         scene.set_property(node_id, "semantic.v1.ground_contact", prop_bool(meta.requires_ground_contact)) &&
         scene.set_property(node_id, "semantic.v1.clearance_m", prop_double(meta.clearance_radius_m)) &&
         scene.set_property(node_id, "semantic.v1.walkway_m", prop_double(meta.min_walkway_clearance_m));
}

std::optional<SemanticMetadata> load_semantic_metadata(const EditorNode& node) {
  const PropertyValue* version = find_prop(node, "semantic.v1.version", PropertyType::Int64);
  if (version == nullptr || version->int_val != 1) return std::nullopt;
  const PropertyValue* role = find_prop(node, "semantic.v1.role", PropertyType::Int64);
  const PropertyValue* front = find_prop(node, "semantic.v1.front", PropertyType::Vec3);
  const PropertyValue* up = find_prop(node, "semantic.v1.up", PropertyType::Vec3);
  const PropertyValue* ground = find_prop(node, "semantic.v1.ground_contact", PropertyType::Bool);
  const PropertyValue* clearance = find_prop(node, "semantic.v1.clearance_m", PropertyType::Double);
  const PropertyValue* walkway = find_prop(node, "semantic.v1.walkway_m", PropertyType::Double);
  if (!role || !front || !up || !ground || !clearance || !walkway) return std::nullopt;
  if (role->int_val < 0 || role->int_val > static_cast<std::int64_t>(SemanticRole::RoadLane)) return std::nullopt;
  SemanticMetadata m;
  m.role = static_cast<SemanticRole>(role->int_val);
  m.local_front_dir = front->vec3_val;
  m.local_up_dir = up->vec3_val;
  m.requires_ground_contact = ground->bool_val;
  m.clearance_radius_m = clearance->double_val;
  m.min_walkway_clearance_m = walkway->double_val;
  if (!validate_semantic_metadata(m)) return std::nullopt;
  return m;
}

}  // namespace saige::editor
