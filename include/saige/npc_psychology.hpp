#pragma once

#include "saige/npc_simulation.hpp"

#include <cstdint>
#include <vector>

namespace saige::psychology {

struct EmotionalState {
  double valence = 0.1;  // [-1.0 negative, +1.0 positive]
  double arousal = 0.2;  // [0.0 calm, 1.0 excited/panicked]
  double fear = 0.0;     // [0.0, 1.0]
  double anger = 0.0;    // [0.0, 1.0]
  double stress = 0.1;   // [0.0, 1.0]

  void step_decay(double dt_seconds) noexcept;
};

struct PersonalHabits {
  world_sim::VenueCategory preferred_category = world_sim::VenueCategory::Hospitality;
  double punctuality = 0.6;      // [0.0, 1.0]
  double risk_tolerance = 0.4;   // [0.0, 1.0]
  double social_battery = 0.7;   // [0.0, 1.0]
  double weather_sensitivity = 0.5;
};

struct ExperienceEntry {
  world_sim::VenueId venue_id = world_sim::kInvalidVenueId;
  world_sim::EntityId actor_id = world_sim::kInvalidEntityId;
  double sentiment = 0.0;  // [-1.0 bad experience, +1.0 good experience]
  double timestamp_sec = 0.0;
};

// Reusable environmental contract for engine weather/time/hazard integration.
struct EnvironmentalCondition {
  double rain_intensity = 0.0;   // [0.0, 1.0]
  double storm_severity = 0.0;   // [0.0, 1.0]
  double temperature_c = 20.0;   // Celsius
  double flood_depth_m = 0.0;    // Local water depth
  double visibility_m = 200.0;
};

enum class ContextualAction : std::uint8_t {
  FollowRoutine = 0,
  SeekShelter = 1,
  AvoidArea = 2,
  FleeHazard = 3,
  EngageSocial = 4,
};

struct PsychologyDecision {
  ContextualAction action = ContextualAction::FollowRoutine;
  double speed_multiplier = 1.0;
  double venue_bias = 0.0;
};

class NpcPsychologyProfile {
 public:
  explicit NpcPsychologyProfile(world_sim::EntityId seed_id = 1);

  void record_experience(const ExperienceEntry& entry);
  void apply_emotional_stimulus(double delta_valence, double delta_fear,
                                double delta_anger, double delta_stress) noexcept;
  void step(double dt_seconds, const EnvironmentalCondition& env) noexcept;

  [[nodiscard]] double venue_affinity(world_sim::VenueId venue_id) const noexcept;
  [[nodiscard]] PsychologyDecision evaluate(
      const EnvironmentalCondition& env,
      world_sim::VenueId candidate_venue) const noexcept;

  [[nodiscard]] const EmotionalState& emotions() const noexcept { return emotions_; }
  [[nodiscard]] const PersonalHabits& habits() const noexcept { return habits_; }
  [[nodiscard]] std::size_t experience_count() const noexcept {
    return experiences_.size();
  }

 private:
  EmotionalState emotions_{};
  PersonalHabits habits_{};
  std::vector<ExperienceEntry> experiences_;
  std::size_t max_experiences_ = 32;
};

}  // namespace saige::psychology
