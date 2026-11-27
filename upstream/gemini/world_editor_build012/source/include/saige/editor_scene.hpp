#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace saige::editor {

using EntityId = std::uint64_t;
inline constexpr EntityId kInvalidEntityId = 0;
inline constexpr double kDefaultChunkSizeM = 64.0;

struct Vec3d {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;  // Right-handed Z-up, meter-based SAIGE coordinate system

  [[nodiscard]] bool operator==(const Vec3d& other) const noexcept = default;
};

struct ChunkCoord {
  std::int32_t x = 0;
  std::int32_t y = 0;

  [[nodiscard]] bool operator==(const ChunkCoord& other) const noexcept = default;
};

struct ChunkCoordHash {
  [[nodiscard]] std::size_t operator()(const ChunkCoord& c) const noexcept {
    const std::uint64_t ux = static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x));
    const std::uint64_t uy = static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.y));
    return static_cast<std::size_t>((ux * 0x9E3779B97F4A7C15ULL) ^
                                    (uy * 0xBF58476D1CE4E5B9ULL));
  }
};

struct Transform3d {
  Vec3d position{};
  Vec3d rotation_deg{};
  Vec3d scale{1.0, 1.0, 1.0};

  [[nodiscard]] bool is_valid() const noexcept;
};

struct SnapSettings {
  bool grid_snap_enabled = true;
  double grid_step_m = 0.5;
  bool angle_snap_enabled = true;
  double angle_step_deg = 15.0;

  [[nodiscard]] Transform3d apply(const Transform3d& t) const noexcept;
};

enum class EditorNodeKind : std::uint8_t {
  Group = 0,
  Prop = 1,
  Building = 2,
  Road = 3,
  TerrainPatch = 4,
  NavVolume = 5,
  SpawnPoint = 6,
  Light = 7,
  Vegetation = 8,
};

struct AssetReference {
  std::string asset_uri;
  std::uint32_t asset_version = 1;
  std::uint64_t content_hash = 0;
};

enum class PropertyType : std::uint8_t {
  Bool = 0,
  Int64 = 1,
  Double = 2,
  String = 3,
  Vec3 = 4,
};

struct PropertyValue {
  PropertyType type = PropertyType::Double;
  bool bool_val = false;
  std::int64_t int_val = 0;
  double double_val = 0.0;
  std::string str_val;
  Vec3d vec3_val{};
};

struct PropertyEntry {
  std::string key;
  PropertyValue value{};
};

struct EditorNode {
  EntityId id = kInvalidEntityId;
  EntityId parent_id = kInvalidEntityId;
  std::vector<EntityId> children;
  std::string name;
  EditorNodeKind kind = EditorNodeKind::Prop;
  Transform3d local_transform{};
  AssetReference asset{};
  std::vector<PropertyEntry> properties;
  ChunkCoord chunk{};
  bool visible = true;
  bool locked = false;
};

enum class DiagnosticSeverity : std::uint8_t {
  Info = 0,
  Warning = 1,
  Error = 2,
};

struct ValidationDiagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Info;
  EntityId node_id = kInvalidEntityId;
  std::string code;
  std::string message;
};

class SelectionSet {
 public:
  bool select(EntityId id);
  bool deselect(EntityId id);
  void clear() noexcept;
  [[nodiscard]] bool contains(EntityId id) const noexcept;
  [[nodiscard]] const std::vector<EntityId>& ids() const noexcept { return ids_; }
  [[nodiscard]] std::size_t size() const noexcept { return ids_.size(); }

 private:
  std::vector<EntityId> ids_;
};

class EditorScene {
 public:
  explicit EditorScene(double chunk_size_m = kDefaultChunkSizeM);

  [[nodiscard]] EntityId allocate_id() noexcept { return next_id_++; }
  void set_next_id(EntityId next_id) noexcept { next_id_ = next_id; }
  [[nodiscard]] EntityId next_id() const noexcept { return next_id_; }
  [[nodiscard]] double chunk_size_m() const noexcept { return chunk_size_m_; }

  bool insert_node(EditorNode node);
  bool remove_node_only(EntityId id);
  bool reparent(EntityId node_id, EntityId new_parent_id);
  bool set_transform(EntityId node_id, const Transform3d& t);
  bool set_property(EntityId node_id, const std::string& key,
                    const PropertyValue& val);

  [[nodiscard]] bool is_ancestor_of(EntityId ancestor,
                                    EntityId descendant) const noexcept;
  [[nodiscard]] EditorNode* find_node(EntityId id) noexcept;
  [[nodiscard]] const EditorNode* find_node(EntityId id) const noexcept;

  [[nodiscard]] ChunkCoord compute_chunk_for_position(
      const Vec3d& pos) const noexcept;
  [[nodiscard]] std::vector<EntityId> query_chunk(ChunkCoord chunk) const;
  [[nodiscard]] std::vector<EntityId> collect_subtree(EntityId root_id) const;

  [[nodiscard]] std::vector<ValidationDiagnostic> validate() const;

  [[nodiscard]] const std::vector<EditorNode>& nodes() const noexcept {
    return nodes_;
  }
  [[nodiscard]] SelectionSet& selection() noexcept { return selection_; }
  [[nodiscard]] const SelectionSet& selection() const noexcept {
    return selection_;
  }
  [[nodiscard]] SnapSettings& snap_settings() noexcept { return snap_; }
  [[nodiscard]] const SnapSettings& snap_settings() const noexcept {
    return snap_;
  }

 private:
  void rebuild_indices() const;

  double chunk_size_m_ = kDefaultChunkSizeM;
  EntityId next_id_ = 1;
  std::vector<EditorNode> nodes_;
  SelectionSet selection_;
  SnapSettings snap_{};

  mutable std::unordered_map<EntityId, std::size_t> id_to_index_;
  mutable std::unordered_map<ChunkCoord, std::vector<EntityId>, ChunkCoordHash>
      chunk_to_ids_;
  mutable bool indices_dirty_ = false;
};

}  // namespace saige::editor
