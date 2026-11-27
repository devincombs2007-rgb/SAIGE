#pragma once

#include "saige/npc_simulation.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace saige::persistent_world {

inline constexpr std::uint32_t kFormatMagicV1 = 0x574C4431u;  // "WLD1" (Build 008)
inline constexpr std::uint32_t kFormatMagic = 0x574C4432u;    // "WLD2" (Build 009+)
inline constexpr std::uint16_t kFormatVersion = 3;  // v3 (SAIGE, 2026-11-27): business cash and expenses and the economy treasury, so a save keeps the ledger
inline constexpr std::uint64_t kMaxSerializedRecords = 2000000ULL;

enum class RelationshipKind : std::uint8_t {
  Acquaintance = 0,
  Friend = 1,
  Family = 2,
  Coworker = 3,
  Rival = 4,
};

struct RelationshipRecord {
  world_sim::EntityId from_id = world_sim::kInvalidEntityId;
  world_sim::EntityId to_id = world_sim::kInvalidEntityId;
  RelationshipKind kind = RelationshipKind::Acquaintance;
  float affinity = 0.0f;     // [-1.0, 1.0]
  float familiarity = 0.0f;  // [0.0, 1.0]
  float trust = 0.0f;        // [-1.0, 1.0]
  std::uint32_t interaction_count = 0;
  double last_interaction_sec = 0.0;
};

struct BusinessRecord {
  world_sim::VenueId venue_id = world_sim::kInvalidVenueId;
  world_sim::VenueCategory category = world_sim::VenueCategory::Commercial;
  world_sim::WorldPosition anchor{};
  std::uint16_t open_minute = 480;
  std::uint16_t close_minute = 1080;
  std::uint32_t inventory_units = 100;
  double unit_price = 12.0;
  double demand_multiplier = 1.0;
  std::uint64_t cumulative_revenue_cents = 0;
  std::int64_t cash_cents = 0;       ///< v3: the business till (a v1 or v2 save loads 0)
  std::uint64_t expense_cents = 0;   ///< v3: cumulative wages and wholesale paid
};

struct PersistentNpcRecord {
  world_sim::EntityId id = world_sim::kInvalidEntityId;
  world_sim::WorldPosition position{};
  world_sim::SimLodTier lod = world_sim::SimLodTier::Dormant;
  world_sim::OccupationKind occupation = world_sim::OccupationKind::Unemployed;
  world_sim::VenueId home_venue = world_sim::kInvalidVenueId;
  world_sim::VenueId work_venue = world_sim::kInvalidVenueId;
  std::uint64_t owned_vehicle_id = 0;
  std::int64_t balance_cents = 50000;
  float bravery = 0.5f;
  float sociability = 0.5f;
  float conscientiousness = 0.5f;
  float energy = 0.85f;
  float hunger = 0.15f;
  float stress = 0.10f;
  double last_simulated_sec = 0.0;
  std::uint64_t evaluation_count = 0;
};

struct PersistentEventRecord {
  std::uint32_t id = 0;
  world_sim::SensoryKind kind = world_sim::SensoryKind::EmergencyAlert;
  world_sim::WorldPosition position{};
  double radius_m = 40.0;
  double intensity = 0.8;
  double expires_at_sec = 0.0;
};

struct StoreStepMetrics {
  std::uint32_t total_records = 0;
  std::uint32_t active_records = 0;
  std::uint32_t background_records = 0;
  std::uint32_t dormant_records = 0;
  std::uint32_t evaluated_this_tick = 0;
  std::uint32_t caught_up_on_stream_in = 0;
  double max_decision_latency_sec = 0.0;
  double index_rebuild_ms = 0.0;
  std::size_t estimated_memory_bytes = 0;
};

class PersistentWorldStore {
 public:
  explicit PersistentWorldStore(double chunk_size_m = 64.0,
                                double active_radius_m = 80.0,
                                double background_radius_m = 220.0);

  void reserve_npcs(std::size_t count);
  bool upsert_npc(const PersistentNpcRecord& rec);
  [[nodiscard]] const PersistentNpcRecord* find_npc(
      world_sim::EntityId id) const noexcept;
  bool set_npc_position(world_sim::EntityId id,
                        const world_sim::WorldPosition& pos);
  bool set_npc_balance(world_sim::EntityId id, std::int64_t balance_cents);

  bool upsert_relationship(const RelationshipRecord& rel);
  [[nodiscard]] const RelationshipRecord* find_relationship(
      world_sim::EntityId from_id, world_sim::EntityId to_id) const noexcept;

  bool upsert_business(const BusinessRecord& biz);
  [[nodiscard]] const BusinessRecord* find_business(
      world_sim::VenueId id) const noexcept;

  bool add_event(const PersistentEventRecord& ev);

  void query_chunk_npcs(world_sim::ChunkCoord chunk,
                        std::vector<std::size_t>& out_indices) const;

  StoreStepMetrics step_focused(double dt_seconds,
                                const world_sim::WorldPosition& focus_pos,
                                std::uint32_t max_active_evals = 128,
                                std::uint32_t max_bg_evals = 64);

  std::uint32_t catch_up_chunk(world_sim::ChunkCoord chunk,
                               double current_time_sec);

  // Explicit field-based little-endian serialization with CRC-32 checksum &
  // bounded allocation guards (supports v1 -> v2 migration).
  /// The economy's supplier treasury, kept with the save so that a save and load conserves the total money (v3). Unset (`has_economy_treasury()` false) in a
  /// store that never received one and after loading a v1 or v2 save.
  void set_economy_treasury(std::int64_t cents) noexcept { economy_treasury_cents_ = cents; has_economy_treasury_ = true; }
  [[nodiscard]] bool has_economy_treasury() const noexcept { return has_economy_treasury_; }
  [[nodiscard]] std::int64_t economy_treasury_cents() const noexcept { return economy_treasury_cents_; }
  [[nodiscard]] std::vector<std::uint8_t> serialize() const;
  [[nodiscard]] bool deserialize(const std::vector<std::uint8_t>& blob);

  [[nodiscard]] std::size_t npc_count() const noexcept { return npcs_.size(); }
  [[nodiscard]] std::size_t relationship_count() const noexcept {
    return relationships_.size();
  }
  [[nodiscard]] std::size_t business_count() const noexcept {
    return businesses_.size();
  }
  [[nodiscard]] std::size_t event_count() const noexcept {
    return events_.size();
  }
  [[nodiscard]] double current_time_sec() const noexcept {
    return current_time_sec_;
  }
  [[nodiscard]] const std::vector<PersistentNpcRecord>& npcs() const noexcept {
    return npcs_;
  }
  [[nodiscard]] const std::vector<RelationshipRecord>& relationships() const noexcept {
    return relationships_;
  }

 private:
  std::int64_t economy_treasury_cents_ = 0;
  bool has_economy_treasury_ = false;
  struct ChunkBucket {
    world_sim::ChunkCoord coord{};
    std::vector<std::size_t> npc_indices;
  };

  void ensure_spatial_index() const;
  void rebuild_spatial_index() const;
  [[nodiscard]] std::size_t find_bucket_idx(
      world_sim::ChunkCoord c) const noexcept;

  double chunk_size_m_ = 64.0;
  double active_radius_m_ = 80.0;
  double background_radius_m_ = 220.0;
  double current_time_sec_ = 0.0;

  std::vector<PersistentNpcRecord> npcs_;
  std::vector<RelationshipRecord> relationships_;
  std::vector<BusinessRecord> businesses_;
  std::vector<PersistentEventRecord> events_;

  mutable std::vector<ChunkBucket> chunk_buckets_;
  mutable std::vector<bool> bucket_occupied_;
  mutable bool spatial_index_dirty_ = true;
  mutable double last_index_rebuild_ms_ = 0.0;

  std::vector<std::size_t> prev_focused_slots_;
  std::size_t active_cursor_ = 0;
  std::size_t bg_cursor_ = 0;
};

}  // namespace saige::persistent_world
