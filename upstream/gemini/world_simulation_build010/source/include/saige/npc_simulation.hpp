#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace saige::world_sim {

using EntityId = std::uint64_t;
using VenueId = std::uint32_t;

inline constexpr EntityId kInvalidEntityId = 0;
inline constexpr VenueId kInvalidVenueId = 0;
inline constexpr std::uint64_t kFixedTickNs = 16666667ULL;
inline constexpr double kDefaultChunkSizeM = 64.0;

struct Vec3f {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

struct Vec3d {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
};

// SAIGE uses Z-up with an X/Y horizontal chunk grid.
struct ChunkCoord {
  std::int32_t x = 0;
  std::int32_t y = 0;

  [[nodiscard]] bool operator==(const ChunkCoord& other) const noexcept = default;
};

// Compatible with SAIGE's large-world WorldPosition (X/Y cell + chunk-local
// float, Z-up) using double arithmetic for relative distances without overflow.
struct WorldPosition {
  ChunkCoord cell{};
  Vec3f local{};

  [[nodiscard]] Vec3d to_world_double(
      double chunk_size_m = kDefaultChunkSizeM) const noexcept;
  [[nodiscard]] static WorldPosition from_world_double(
      const Vec3d& pos, double chunk_size_m = kDefaultChunkSizeM) noexcept;
  [[nodiscard]] static double distance(
      const WorldPosition& a, const WorldPosition& b,
      double chunk_size_m = kDefaultChunkSizeM) noexcept;
};

enum class SimLodTier : std::uint8_t {
  Active = 0,
  Background = 1,
  Dormant = 2,
};

enum class VenueCategory : std::uint8_t {
  Residential = 0,
  Commercial = 1,
  Industrial = 2,
  Civic = 3,
  Hospitality = 4,
  EmergencyStation = 5,
  TransitHub = 6,
};

enum class NpcIntent : std::uint8_t {
  Idle = 0,
  Rest = 1,
  Work = 2,
  Shop = 3,
  Socialize = 4,
  Commute = 5,
  Investigate = 6,
  Flee = 7,
  Assist = 8,
};

enum class OccupationKind : std::uint8_t {
  Unemployed = 0,
  Shopkeeper = 1,
  OfficeWorker = 2,
  NightGuard = 3,
  Paramedic = 4,
};

enum class SensoryKind : std::uint8_t {
  Visual = 0,
  Auditory = 1,
  PhysicalDisturbance = 2,
  SocialGreeting = 3,
  EmergencyAlert = 4,
};

struct WorldClock {
  std::uint64_t total_ticks = 0;
  double total_seconds = 0.0;
  double time_scale = 60.0;  // 1 real second = 1 in-game minute
  std::uint32_t day_index = 0;
  double minute_of_day = 480.0;  // Starts at 08:00

  void advance(double dt_seconds) noexcept;
  [[nodiscard]] std::uint16_t current_minute_int() const noexcept;
};

struct Venue {
  VenueId id = kInvalidVenueId;
  VenueCategory category = VenueCategory::Commercial;
  WorldPosition anchor_position{};
  std::uint16_t open_minute = 480;    // 08:00
  std::uint16_t close_minute = 1080;  // 18:00
  std::uint32_t capacity = 16;
  std::uint32_t current_occupants = 0;
  std::uint32_t staff_required = 0;
  std::uint32_t active_staff = 0;
  std::uint32_t active_customers = 0;
  std::uint64_t revenue_units = 0;

  [[nodiscard]] bool is_open(std::uint16_t minute_of_day) const noexcept;
  [[nodiscard]] bool has_capacity() const noexcept;
};

struct PersonalityTraits {
  double bravery = 0.5;
  double sociability = 0.5;
  double conscientiousness = 0.5;
  double curiosity = 0.5;
  double aggression = 0.1;
};

struct NpcDrives {
  double energy = 0.85;
  double hunger = 0.15;
  double social = 0.50;
  double safety = 1.00;
};

struct ScheduleBlock {
  std::uint16_t start_minute = 0;
  std::uint16_t end_minute = 1440;
  VenueId target_venue = kInvalidVenueId;
  NpcIntent intent = NpcIntent::Idle;

  [[nodiscard]] bool contains_minute(std::uint16_t minute_of_day) const noexcept;
};

struct SensoryEvent {
  SensoryKind kind = SensoryKind::Visual;
  EntityId source_id = kInvalidEntityId;
  WorldPosition position{};
  double intensity = 0.0;  // [0.0, 1.0]
  double timestamp_sec = 0.0;
  double ttl_sec = 15.0;
};

struct PersistentMemory {
  VenueId home_venue = kInvalidVenueId;
  VenueId work_venue = kInvalidVenueId;
  EntityId last_threat_source = kInvalidEntityId;
  double threat_memory_strength = 0.0;
  std::uint32_t disturbance_count = 0;
};

struct NavigationIntent {
  bool has_destination = false;
  WorldPosition target_position{};
  double desired_speed = 1.35;  // m/s
  bool urgent = false;
  NpcIntent intent = NpcIntent::Idle;
  VenueId target_venue = kInvalidVenueId;
};

struct NpcAgent {
  EntityId id = kInvalidEntityId;
  WorldPosition position{};
  SimLodTier lod = SimLodTier::Active;
  PersonalityTraits personality{};
  NpcDrives drives{};
  PersistentMemory persistent_memory{};
  std::vector<ScheduleBlock> schedule;
  std::vector<SensoryEvent> short_term_memory;
  NavigationIntent current_nav_intent{};
  VenueId reserved_venue = kInvalidVenueId;
  double last_eval_time_sec = -1.0;
  struct ReactedWorldEvent {
    std::uint32_t event_id = 0;
    double expires_at_sec = 0.0;
  };
  OccupationKind occupation = OccupationKind::Unemployed;
  std::vector<ReactedWorldEvent> reacted_world_events;
  std::uint64_t world_event_reaction_count = 0;
  std::uint64_t evaluation_count = 0;
  double vision_range_m = 25.0;
  double hearing_range_m = 35.0;
  std::size_t max_short_term_memories = 8;
};

struct SimBudgetConfig {
  double chunk_size_m = kDefaultChunkSizeM;
  double active_radius_m = 60.0;
  double background_radius_m = 200.0;
  double lod_hysteresis_m = 5.0;
  double venue_arrival_radius_m = 5.0;
  std::uint32_t max_active_evals_per_tick = 128;
  std::uint32_t max_background_evals_per_tick = 64;
  std::uint32_t max_dormant_evals_per_tick = 16;
  double background_eval_interval_sec = 0.5;
  double dormant_eval_interval_sec = 5.0;
};

struct WorldEventRecord {
  std::uint32_t id = 0;
  SensoryKind kind = SensoryKind::EmergencyAlert;
  WorldPosition position{};
  double radius_m = 40.0;
  double intensity = 0.85;
  double expires_at_sec = 60.0;
};

struct SimStepMetrics {
  std::uint32_t total_npcs = 0;
  std::uint32_t active_count = 0;
  std::uint32_t background_count = 0;
  std::uint32_t dormant_count = 0;
  std::uint32_t evaluated_this_tick = 0;
  std::uint32_t intent_changes = 0;
  std::uint32_t venue_capacity_denials = 0;
};

class WorldSimulation {
 public:
  explicit WorldSimulation(SimBudgetConfig config = {});

  [[nodiscard]] WorldClock& clock() noexcept { return clock_; }
  [[nodiscard]] const WorldClock& clock() const noexcept { return clock_; }

  bool register_venue(const Venue& venue);
  [[nodiscard]] const Venue* find_venue(VenueId id) const noexcept;
  [[nodiscard]] Venue* find_venue_mut(VenueId id) noexcept;

  bool register_npc(NpcAgent agent);
  bool remove_npc(EntityId id);
  [[nodiscard]] NpcAgent* find_npc(EntityId id) noexcept;
  [[nodiscard]] const NpcAgent* find_npc(EntityId id) const noexcept;

  void set_chunk_loaded(ChunkCoord chunk, bool loaded);
  [[nodiscard]] bool is_chunk_loaded(ChunkCoord chunk) const noexcept;

  std::uint32_t emit_sensory_event(const SensoryEvent& event);
  bool register_world_event(const WorldEventRecord& event);
  bool report_traffic_accident(std::uint32_t event_id, const WorldPosition& pos, double duration_sec = 30.0);
  static void assign_occupation_schedule(NpcAgent& npc, OccupationKind occupation, VenueId work_venue, VenueId home_venue);
  [[nodiscard]] const std::vector<WorldEventRecord>& world_events() const noexcept { return world_events_; }
  SimStepMetrics step(double dt_seconds, const WorldPosition& focus_position);

  [[nodiscard]] const std::vector<NpcAgent>& npcs() const noexcept { return npcs_; }
  [[nodiscard]] const std::vector<Venue>& venues() const noexcept { return venues_; }

 private:
  [[nodiscard]] SimLodTier compute_lod_with_hysteresis(
      SimLodTier current, double distance_m) const noexcept;
  void update_drives_and_memory(NpcAgent& npc, double dt_seconds) const;
  void release_venue_reservation(NpcAgent& npc) noexcept;
  bool try_reserve_venue(NpcAgent& npc, VenueId venue_id) noexcept;
  bool evaluate_npc_decision(NpcAgent& npc, SimStepMetrics& metrics);

  SimBudgetConfig config_{};
  WorldClock clock_{};
  std::vector<Venue> venues_;
  std::vector<NpcAgent> npcs_;
  std::vector<WorldEventRecord> world_events_;
  std::vector<ChunkCoord> unloaded_chunks_;
  std::size_t active_cursor_ = 0;
  std::size_t bg_cursor_ = 0;
  std::size_t dormant_cursor_ = 0;
};

}  // namespace saige::world_sim
