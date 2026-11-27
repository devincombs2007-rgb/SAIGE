#include "saige/editor_commands.hpp"

#include <algorithm>

namespace saige::editor {

namespace {

class CompositeTransactionCommand final : public IEditorCommand {
 public:
  CompositeTransactionCommand(std::string label,
                              std::vector<std::unique_ptr<IEditorCommand>> cmds)
      : label_(std::move(label)), cmds_(std::move(cmds)) {}

  [[nodiscard]] const char* label() const noexcept override {
    return label_.c_str();
  }

  bool execute(EditorScene& scene) override {
    for (std::size_t i = 0; i < cmds_.size(); ++i) {
      if (!cmds_[i]->execute(scene)) {
        for (std::size_t j = i; j > 0; --j) {
          cmds_[j - 1]->undo(scene);
        }
        return false;
      }
    }
    return true;
  }

  void undo(EditorScene& scene) override {
    for (std::size_t i = cmds_.size(); i > 0; --i) {
      cmds_[i - 1]->undo(scene);
    }
  }

 private:
  std::string label_;
  std::vector<std::unique_ptr<IEditorCommand>> cmds_;
};

}  // namespace

CommandHistory::CommandHistory(std::size_t max_depth)
    : max_depth_(max_depth > 0 ? max_depth : 1) {}

void CommandHistory::push_undo(std::unique_ptr<IEditorCommand> cmd) {
  if (undo_stack_.size() >= max_depth_) {
    undo_stack_.erase(undo_stack_.begin());
  }
  undo_stack_.push_back(std::move(cmd));
  redo_stack_.clear();
}

bool CommandHistory::execute(EditorScene& scene,
                             std::unique_ptr<IEditorCommand> cmd) {
  if (!cmd || !cmd->execute(scene)) return false;
  if (in_transaction_) {
    tx_commands_.push_back(std::move(cmd));
  } else {
    push_undo(std::move(cmd));
  }
  return true;
}

bool CommandHistory::undo(EditorScene& scene) {
  if (in_transaction_ || undo_stack_.empty()) return false;
  auto cmd = std::move(undo_stack_.back());
  undo_stack_.pop_back();
  cmd->undo(scene);
  redo_stack_.push_back(std::move(cmd));
  return true;
}

bool CommandHistory::redo(EditorScene& scene) {
  if (in_transaction_ || redo_stack_.empty()) return false;
  auto cmd = std::move(redo_stack_.back());
  redo_stack_.pop_back();
  if (!cmd->execute(scene)) return false;
  undo_stack_.push_back(std::move(cmd));
  return true;
}

bool CommandHistory::begin_transaction(std::string label) {
  if (in_transaction_) return false;
  in_transaction_ = true;
  tx_label_ = std::move(label);
  tx_commands_.clear();
  return true;
}

bool CommandHistory::commit_transaction() {
  if (!in_transaction_) return false;
  in_transaction_ = false;
  if (!tx_commands_.empty()) {
    push_undo(std::make_unique<CompositeTransactionCommand>(
        std::move(tx_label_), std::move(tx_commands_)));
  }
  tx_label_.clear();
  tx_commands_.clear();
  return true;
}

bool CommandHistory::rollback_transaction(EditorScene& scene) {
  if (!in_transaction_) return false;
  for (std::size_t i = tx_commands_.size(); i > 0; --i) {
    tx_commands_[i - 1]->undo(scene);
  }
  in_transaction_ = false;
  tx_label_.clear();
  tx_commands_.clear();
  return true;
}

CreateNodeCommand::CreateNodeCommand(EditorNode node) : node_(std::move(node)) {}

bool CreateNodeCommand::execute(EditorScene& scene) {
  if (node_.id == kInvalidEntityId) {
    node_.id = scene.allocate_id();
  }
  return scene.insert_node(node_);
}

void CreateNodeCommand::undo(EditorScene& scene) {
  scene.remove_node_only(node_.id);
}

DeleteSubtreeCommand::DeleteSubtreeCommand(EntityId root_id)
    : root_id_(root_id) {}

bool DeleteSubtreeCommand::execute(EditorScene& scene) {
  const EditorNode* root = scene.find_node(root_id_);
  if (root == nullptr || root->locked) return false;
  const auto ids = scene.collect_subtree(root_id_);
  saved_nodes_.clear();
  saved_nodes_.reserve(ids.size());
  for (EntityId id : ids) {
    if (const EditorNode* n = scene.find_node(id)) {
      if (n->locked) return false;
      saved_nodes_.push_back(*n);
    }
  }
  // Remove leaves first (reverse BFS/DFS order)
  for (std::size_t i = saved_nodes_.size(); i > 0; --i) {
    scene.remove_node_only(saved_nodes_[i - 1].id);
  }
  return true;
}

void DeleteSubtreeCommand::undo(EditorScene& scene) {
  // Restore parents before children (preorder)
  for (auto node : saved_nodes_) {
    node.children.clear();
    scene.insert_node(std::move(node));
  }
}

SetTransformCommand::SetTransformCommand(EntityId node_id,
                                         Transform3d new_transform,
                                         bool apply_snap)
    : node_id_(node_id),
      new_transform_(new_transform),
      apply_snap_(apply_snap) {}

bool SetTransformCommand::execute(EditorScene& scene) {
  const EditorNode* n = scene.find_node(node_id_);
  if (n == nullptr) return false;
  old_transform_ = n->local_transform;
  const Transform3d final_t =
      apply_snap_ ? scene.snap_settings().apply(new_transform_) : new_transform_;
  return scene.set_transform(node_id_, final_t);
}

void SetTransformCommand::undo(EditorScene& scene) {
  scene.set_transform(node_id_, old_transform_);
}

ReparentCommand::ReparentCommand(EntityId node_id, EntityId new_parent_id)
    : node_id_(node_id), new_parent_id_(new_parent_id) {}

bool ReparentCommand::execute(EditorScene& scene) {
  const EditorNode* n = scene.find_node(node_id_);
  if (n == nullptr) return false;
  old_parent_id_ = n->parent_id;
  return scene.reparent(node_id_, new_parent_id_);
}

void ReparentCommand::undo(EditorScene& scene) {
  scene.reparent(node_id_, old_parent_id_);
}

DuplicateSubtreeCommand::DuplicateSubtreeCommand(EntityId source_root_id,
                                                 Vec3d offset)
    : source_root_id_(source_root_id), offset_(offset) {}

bool DuplicateSubtreeCommand::execute(EditorScene& scene) {
  const auto src_ids = scene.collect_subtree(source_root_id_);
  if (src_ids.empty()) return false;

  created_ids_.clear();
  std::vector<std::pair<EntityId, EntityId>> id_map;
  id_map.reserve(src_ids.size());

  for (EntityId old_id : src_ids) {
    id_map.push_back({old_id, scene.allocate_id()});
  }
  duplicated_root_id_ = id_map.front().second;

  auto map_id = [&](EntityId old_id) -> EntityId {
    for (const auto& kv : id_map) {
      if (kv.first == old_id) return kv.second;
    }
    return old_id;
  };

  for (EntityId old_id : src_ids) {
    const EditorNode* src = scene.find_node(old_id);
    if (src == nullptr) return false;
    EditorNode copy = *src;
    copy.id = map_id(old_id);
    copy.children.clear();
    if (old_id == source_root_id_) {
      copy.local_transform.position.x += offset_.x;
      copy.local_transform.position.y += offset_.y;
      copy.local_transform.position.z += offset_.z;
    } else {
      copy.parent_id = map_id(src->parent_id);
    }
    copy.name += "_copy";
    if (!scene.insert_node(std::move(copy))) return false;
    created_ids_.push_back(copy.id);
  }
  return true;
}

void DuplicateSubtreeCommand::undo(EditorScene& scene) {
  for (std::size_t i = created_ids_.size(); i > 0; --i) {
    scene.remove_node_only(created_ids_[i - 1]);
  }
}

GroupNodesCommand::GroupNodesCommand(std::string group_name,
                                     std::vector<EntityId> members)
    : group_name_(std::move(group_name)), members_(std::move(members)) {}

bool GroupNodesCommand::execute(EditorScene& scene) {
  if (members_.empty()) return false;
  old_parents_.clear();
  for (EntityId id : members_) {
    const EditorNode* n = scene.find_node(id);
    if (n == nullptr || n->locked) return false;
    old_parents_.push_back(n->parent_id);
  }

  EditorNode grp{};
  grp.id = (group_id_ != kInvalidEntityId) ? group_id_ : scene.allocate_id();
  group_id_ = grp.id;
  grp.name = group_name_.empty() ? "Group" : group_name_;
  grp.kind = EditorNodeKind::Group;
  if (!scene.insert_node(std::move(grp))) return false;

  for (EntityId id : members_) {
    if (!scene.reparent(id, group_id_)) {
      undo(scene);
      return false;
    }
  }
  return true;
}

void GroupNodesCommand::undo(EditorScene& scene) {
  for (std::size_t i = 0; i < members_.size() && i < old_parents_.size(); ++i) {
    scene.reparent(members_[i], old_parents_[i]);
  }
  if (group_id_ != kInvalidEntityId) {
    scene.remove_node_only(group_id_);
  }
}

}  // namespace saige::editor
