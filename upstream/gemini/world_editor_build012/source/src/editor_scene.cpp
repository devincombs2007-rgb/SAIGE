#include "saige/editor_scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace saige::editor {

namespace {

[[nodiscard]] bool is_finite_vec3(const Vec3d& v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] double snap_scalar(double val, double step) noexcept {
  if (!std::isfinite(val) || !std::isfinite(step) || step <= 0.0) return val;
  return std::round(val / step) * step;
}

}  // namespace

bool Transform3d::is_valid() const noexcept {
  return is_finite_vec3(position) && is_finite_vec3(rotation_deg) &&
         is_finite_vec3(scale) && scale.x > 0.0 && scale.y > 0.0 &&
         scale.z > 0.0;
}

Transform3d SnapSettings::apply(const Transform3d& t) const noexcept {
  Transform3d out = t;
  if (grid_snap_enabled && grid_step_m > 0.0) {
    out.position.x = snap_scalar(out.position.x, grid_step_m);
    out.position.y = snap_scalar(out.position.y, grid_step_m);
    out.position.z = snap_scalar(out.position.z, grid_step_m);
  }
  if (angle_snap_enabled && angle_step_deg > 0.0) {
    out.rotation_deg.x = snap_scalar(out.rotation_deg.x, angle_step_deg);
    out.rotation_deg.y = snap_scalar(out.rotation_deg.y, angle_step_deg);
    out.rotation_deg.z = snap_scalar(out.rotation_deg.z, angle_step_deg);
  }
  return out;
}

bool SelectionSet::select(EntityId id) {
  if (id == kInvalidEntityId || contains(id)) return false;
  ids_.push_back(id);
  std::sort(ids_.begin(), ids_.end());
  return true;
}

bool SelectionSet::deselect(EntityId id) {
  const auto it = std::find(ids_.begin(), ids_.end(), id);
  if (it == ids_.end()) return false;
  ids_.erase(it);
  return true;
}

void SelectionSet::clear() noexcept { ids_.clear(); }

bool SelectionSet::contains(EntityId id) const noexcept {
  return std::binary_search(ids_.begin(), ids_.end(), id);
}

EditorScene::EditorScene(double chunk_size_m)
    : chunk_size_m_(
          (std::isfinite(chunk_size_m) && chunk_size_m > 1.0)
              ? chunk_size_m
              : kDefaultChunkSizeM) {}

void EditorScene::rebuild_indices() const {
  if (!indices_dirty_) return;
  id_to_index_.clear();
  chunk_to_ids_.clear();
  id_to_index_.reserve(nodes_.size());
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    id_to_index_[nodes_[i].id] = i;
    chunk_to_ids_[nodes_[i].chunk].push_back(nodes_[i].id);
  }
  indices_dirty_ = false;
}

ChunkCoord EditorScene::compute_chunk_for_position(
    const Vec3d& pos) const noexcept {
  if (!std::isfinite(pos.x) || !std::isfinite(pos.y)) return {};
  return {
      static_cast<std::int32_t>(std::floor(pos.x / chunk_size_m_)),
      static_cast<std::int32_t>(std::floor(pos.y / chunk_size_m_)),
  };
}

bool EditorScene::insert_node(EditorNode node) {
  if (node.id == kInvalidEntityId || find_node(node.id) != nullptr) {
    return false;
  }
  if (node.parent_id != kInvalidEntityId) {
    EditorNode* parent = find_node(node.parent_id);
    if (parent == nullptr) return false;
    if (std::find(parent->children.begin(), parent->children.end(), node.id) ==
        parent->children.end()) {
      parent->children.push_back(node.id);
    }
  }
  node.chunk = compute_chunk_for_position(node.local_transform.position);
  if (node.id >= next_id_) {
    next_id_ = node.id + 1;
  }
  const std::size_t new_idx = nodes_.size();
  const EntityId new_id = node.id;
  const ChunkCoord new_chunk = node.chunk;
  nodes_.push_back(std::move(node));
  if (!indices_dirty_) {
    id_to_index_[new_id] = new_idx;
    chunk_to_ids_[new_chunk].push_back(new_id);
  }
  return true;
}

bool EditorScene::remove_node_only(EntityId id) {
  rebuild_indices();
  const auto map_it = id_to_index_.find(id);
  if (map_it == id_to_index_.end()) return false;
  const std::size_t idx = map_it->second;
  const EntityId parent_id = nodes_[idx].parent_id;

  if (parent_id != kInvalidEntityId) {
    if (EditorNode* parent = find_node(parent_id)) {
      parent->children.erase(
          std::remove(parent->children.begin(), parent->children.end(), id),
          parent->children.end());
    }
  }
  selection_.deselect(id);
  nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(idx));
  indices_dirty_ = true;
  return true;
}

bool EditorScene::is_ancestor_of(EntityId ancestor,
                                 EntityId descendant) const noexcept {
  EntityId cur = descendant;
  std::size_t guard = 0;
  while (cur != kInvalidEntityId && guard <= nodes_.size()) {
    if (cur == ancestor) return true;
    const EditorNode* n = find_node(cur);
    if (n == nullptr) break;
    cur = n->parent_id;
    ++guard;
  }
  return false;
}

bool EditorScene::reparent(EntityId node_id, EntityId new_parent_id) {
  EditorNode* node = find_node(node_id);
  if (node == nullptr || node->locked) return false;
  if (new_parent_id == node_id) return false;
  if (new_parent_id != kInvalidEntityId) {
    if (find_node(new_parent_id) == nullptr) return false;
    if (is_ancestor_of(node_id, new_parent_id)) return false;
  }
  if (node->parent_id != kInvalidEntityId) {
    if (EditorNode* old_p = find_node(node->parent_id)) {
      old_p->children.erase(
          std::remove(old_p->children.begin(), old_p->children.end(), node_id),
          old_p->children.end());
    }
  }
  // Re-fetch after potential lookups
  node = find_node(node_id);
  node->parent_id = new_parent_id;
  if (new_parent_id != kInvalidEntityId) {
    if (EditorNode* new_p = find_node(new_parent_id)) {
      new_p->children.push_back(node_id);
    }
  }
  return true;
}

bool EditorScene::set_transform(EntityId node_id, const Transform3d& t) {
  EditorNode* node = find_node(node_id);
  if (node == nullptr || node->locked || !t.is_valid()) return false;
  const ChunkCoord old_chunk = node->chunk;
  const ChunkCoord new_chunk = compute_chunk_for_position(t.position);
  node->local_transform = t;
  node->chunk = new_chunk;
  if (!(old_chunk == new_chunk)) {
    indices_dirty_ = true;
  }
  return true;
}

bool EditorScene::set_property(EntityId node_id, const std::string& key,
                               const PropertyValue& val) {
  EditorNode* node = find_node(node_id);
  if (node == nullptr || node->locked || key.empty()) return false;
  for (auto& prop : node->properties) {
    if (prop.key == key) {
      prop.value = val;
      return true;
    }
  }
  node->properties.push_back({key, val});
  return true;
}

EditorNode* EditorScene::find_node(EntityId id) noexcept {
  rebuild_indices();
  const auto it = id_to_index_.find(id);
  if (it == id_to_index_.end()) return nullptr;
  return &nodes_[it->second];
}

const EditorNode* EditorScene::find_node(EntityId id) const noexcept {
  rebuild_indices();
  const auto it = id_to_index_.find(id);
  if (it == id_to_index_.end()) return nullptr;
  return &nodes_[it->second];
}

std::vector<EntityId> EditorScene::query_chunk(ChunkCoord chunk) const {
  rebuild_indices();
  const auto it = chunk_to_ids_.find(chunk);
  if (it == chunk_to_ids_.end()) return {};
  return it->second;
}

std::vector<EntityId> EditorScene::collect_subtree(EntityId root_id) const {
  std::vector<EntityId> out;
  const EditorNode* root = find_node(root_id);
  if (root == nullptr) return out;
  std::vector<EntityId> stack{root_id};
  while (!stack.empty()) {
    const EntityId cur = stack.back();
    stack.pop_back();
    out.push_back(cur);
    if (const EditorNode* n = find_node(cur)) {
      for (EntityId child : n->children) {
        stack.push_back(child);
      }
    }
  }
  return out;
}

std::vector<ValidationDiagnostic> EditorScene::validate() const {
  std::vector<ValidationDiagnostic> diags;
  for (const auto& n : nodes_) {
    if (!n.local_transform.is_valid()) {
      diags.push_back({DiagnosticSeverity::Error, n.id, "INVALID_TRANSFORM",
                       "Node has non-finite transform or non-positive scale."});
    }
    if ((n.kind == EditorNodeKind::Prop || n.kind == EditorNodeKind::Building ||
         n.kind == EditorNodeKind::Vegetation) &&
        n.asset.asset_uri.empty()) {
      diags.push_back({DiagnosticSeverity::Warning, n.id, "MISSING_ASSET_URI",
                       "Placeable node has an empty asset_uri."});
    }
    if (n.parent_id != kInvalidEntityId && find_node(n.parent_id) == nullptr) {
      diags.push_back({DiagnosticSeverity::Error, n.id, "ORPHAN_PARENT",
                       "Node references non-existent parent_id."});
    }
    const ChunkCoord expected =
        compute_chunk_for_position(n.local_transform.position);
    if (!(expected == n.chunk)) {
      diags.push_back({DiagnosticSeverity::Warning, n.id, "CHUNK_MISMATCH",
                       "Node chunk coordinate out of sync with transform."});
    }
  }
  return diags;
}

}  // namespace saige::editor
