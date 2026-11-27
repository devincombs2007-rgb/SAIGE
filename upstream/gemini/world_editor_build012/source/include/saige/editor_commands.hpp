#pragma once

#include "saige/editor_scene.hpp"

#include <memory>
#include <string>
#include <vector>

namespace saige::editor {

// Unified command interface shared by human GUI actions and AI authoring agents.
class IEditorCommand {
 public:
  virtual ~IEditorCommand() = default;
  [[nodiscard]] virtual const char* label() const noexcept = 0;
  virtual bool execute(EditorScene& scene) = 0;
  virtual void undo(EditorScene& scene) = 0;
};

class CommandHistory {
 public:
  explicit CommandHistory(std::size_t max_depth = 256);

  bool execute(EditorScene& scene, std::unique_ptr<IEditorCommand> cmd);
  bool undo(EditorScene& scene);
  bool redo(EditorScene& scene);

  // Transactional batching: multiple commands undo/redo as one atomic unit,
  // and rollback_transaction() cleanly reverts any partial changes.
  bool begin_transaction(std::string label);
  bool commit_transaction();
  bool rollback_transaction(EditorScene& scene);

  [[nodiscard]] bool in_transaction() const noexcept { return in_transaction_; }
  [[nodiscard]] bool can_undo() const noexcept { return !undo_stack_.empty(); }
  [[nodiscard]] bool can_redo() const noexcept { return !redo_stack_.empty(); }
  [[nodiscard]] std::size_t undo_count() const noexcept {
    return undo_stack_.size();
  }
  [[nodiscard]] std::size_t redo_count() const noexcept {
    return redo_stack_.size();
  }

 private:
  void push_undo(std::unique_ptr<IEditorCommand> cmd);

  std::size_t max_depth_ = 256;
  std::vector<std::unique_ptr<IEditorCommand>> undo_stack_;
  std::vector<std::unique_ptr<IEditorCommand>> redo_stack_;

  bool in_transaction_ = false;
  std::string tx_label_;
  std::vector<std::unique_ptr<IEditorCommand>> tx_commands_;
};

// Concrete validated commands
class CreateNodeCommand final : public IEditorCommand {
 public:
  explicit CreateNodeCommand(EditorNode node);
  [[nodiscard]] const char* label() const noexcept override { return "CreateNode"; }
  bool execute(EditorScene& scene) override;
  void undo(EditorScene& scene) override;
  [[nodiscard]] EntityId created_id() const noexcept { return node_.id; }

 private:
  EditorNode node_;
};

class DeleteSubtreeCommand final : public IEditorCommand {
 public:
  explicit DeleteSubtreeCommand(EntityId root_id);
  [[nodiscard]] const char* label() const noexcept override {
    return "DeleteSubtree";
  }
  bool execute(EditorScene& scene) override;
  void undo(EditorScene& scene) override;

 private:
  EntityId root_id_;
  std::vector<EditorNode> saved_nodes_;
};

class SetTransformCommand final : public IEditorCommand {
 public:
  SetTransformCommand(EntityId node_id, Transform3d new_transform,
                      bool apply_snap = true);
  [[nodiscard]] const char* label() const noexcept override {
    return "SetTransform";
  }
  bool execute(EditorScene& scene) override;
  void undo(EditorScene& scene) override;

 private:
  EntityId node_id_;
  Transform3d new_transform_;
  Transform3d old_transform_{};
  bool apply_snap_ = true;
};

class ReparentCommand final : public IEditorCommand {
 public:
  ReparentCommand(EntityId node_id, EntityId new_parent_id);
  [[nodiscard]] const char* label() const noexcept override { return "Reparent"; }
  bool execute(EditorScene& scene) override;
  void undo(EditorScene& scene) override;

 private:
  EntityId node_id_;
  EntityId new_parent_id_;
  EntityId old_parent_id_ = kInvalidEntityId;
};

class DuplicateSubtreeCommand final : public IEditorCommand {
 public:
  DuplicateSubtreeCommand(EntityId source_root_id, Vec3d offset);
  [[nodiscard]] const char* label() const noexcept override {
    return "DuplicateSubtree";
  }
  bool execute(EditorScene& scene) override;
  void undo(EditorScene& scene) override;
  [[nodiscard]] EntityId duplicated_root_id() const noexcept {
    return duplicated_root_id_;
  }

 private:
  EntityId source_root_id_;
  Vec3d offset_{};
  EntityId duplicated_root_id_ = kInvalidEntityId;
  std::vector<EntityId> created_ids_;
};

class GroupNodesCommand final : public IEditorCommand {
 public:
  GroupNodesCommand(std::string group_name, std::vector<EntityId> members);
  [[nodiscard]] const char* label() const noexcept override {
    return "GroupNodes";
  }
  bool execute(EditorScene& scene) override;
  void undo(EditorScene& scene) override;
  [[nodiscard]] EntityId group_id() const noexcept { return group_id_; }

 private:
  std::string group_name_;
  std::vector<EntityId> members_;
  std::vector<EntityId> old_parents_;
  EntityId group_id_ = kInvalidEntityId;
};

}  // namespace saige::editor
