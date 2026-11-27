#pragma once

#include "saige/npc_psychology.hpp"
#include "saige/npc_simulation.hpp"
#include "saige/persistent_world.hpp"

#include <cstdint>
#include <vector>

namespace saige::social {

enum class SocialAction : std::uint8_t {
  None = 0,
  WaveGreeting = 1,
  FriendlyChat = 2,
  DeepConversation = 3,
  HelpAlly = 4,
  Avoid = 5,
  Confront = 6,
};

struct SocialEdge {
  world_sim::EntityId a = world_sim::kInvalidEntityId;
  world_sim::EntityId b = world_sim::kInvalidEntityId;
  persistent_world::RelationshipKind kind =
      persistent_world::RelationshipKind::Acquaintance;
  float familiarity = 0.0f;  // [0.0, 1.0]
  float affinity = 0.0f;     // [-1.0, 1.0]
  float trust = 0.0f;        // [-1.0, 1.0]
  std::uint32_t interaction_count = 0;
  double last_interaction_sec = -1000.0;
};

struct SocialGathering {
  std::uint32_t id = 0;
  world_sim::VenueId venue_id = world_sim::kInvalidVenueId;
  world_sim::WorldPosition center{};
  std::uint16_t start_minute = 1080;  // 18:00
  std::uint16_t end_minute = 1260;    // 21:00
  std::vector<world_sim::EntityId> participants;

  [[nodiscard]] bool is_active(std::uint16_t minute_of_day) const noexcept;
};

struct SocialEncounterDecision {
  SocialAction action = SocialAction::None;
  double affinity_delta = 0.0;
  double trust_delta = 0.0;
  double emotion_valence_delta = 0.0;
};

class SocialNetwork {
 public:
  bool set_relationship(const SocialEdge& edge);
  [[nodiscard]] const SocialEdge* find_relationship(
      world_sim::EntityId a, world_sim::EntityId b) const noexcept;
  [[nodiscard]] SocialEdge* find_relationship_mut(
      world_sim::EntityId a, world_sim::EntityId b) noexcept;

  // Evaluates contextual social action from relationship + personality + emotion + weather.
  [[nodiscard]] SocialEncounterDecision evaluate_encounter(
      const world_sim::NpcAgent& actor, const world_sim::NpcAgent& target,
      const psychology::NpcPsychologyProfile& actor_psych,
      const psychology::EnvironmentalCondition& env,
      double current_time_sec,
      double max_encounter_distance_m = 6.0) const noexcept;

  // Executes encounter, updating relationship (Acquaintance->Friend/Rival),
  // psychology experience/emotions, and optionally persistent world records.
  bool apply_encounter(world_sim::NpcAgent& actor, world_sim::NpcAgent& target,
                       psychology::NpcPsychologyProfile& actor_psych,
                       const psychology::EnvironmentalCondition& env,
                       double current_time_sec,
                       double max_encounter_distance_m = 6.0);

  bool add_gathering(const SocialGathering& gathering);
  [[nodiscard]] world_sim::WorldPosition compute_group_formation_slot(
      const world_sim::WorldPosition& leader_pos, std::size_t member_idx,
      std::size_t group_size, double chunk_size_m = 64.0) const noexcept;

  // Connects active social gatherings directly to NpcAgent schedule/navigation intents.
  std::uint32_t apply_gathering_schedules(
      std::vector<world_sim::NpcAgent>& npcs,
      std::uint16_t minute_of_day) const;

  void sync_to_persistent_store(
      persistent_world::PersistentWorldStore& store) const;
  void load_from_persistent_store(
      const persistent_world::PersistentWorldStore& store);

  [[nodiscard]] std::size_t edge_count() const noexcept { return edges_.size(); }

 private:
  std::vector<SocialEdge> edges_;
  std::vector<SocialGathering> gatherings_;
};

}  // namespace saige::social
