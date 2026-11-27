#include "saige/npc_psychology.hpp"

#include <algorithm>
#include <cmath>

namespace saige::psychology {

namespace {

[[nodiscard]] double clamp(double v, double lo, double hi) noexcept {
  if (!std::isfinite(v)) return lo;
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

[[nodiscard]] double hash_unit(std::uint64_t x) noexcept {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdULL;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ULL;
  x ^= x >> 33;
  return static_cast<double>(x & 0xFFFFu) / 65535.0;
}

}  // namespace

void EmotionalState::step_decay(double dt_seconds) noexcept {
  if (!std::isfinite(dt_seconds) || dt_seconds <= 0.0) return;
  const double rate = std::min(1.0, dt_seconds * 0.05);
  valence += (0.1 - valence) * rate;
  arousal += (0.2 - arousal) * rate;
  fear = clamp(fear - 0.04 * dt_seconds, 0.0, 1.0);
  anger = clamp(anger - 0.03 * dt_seconds, 0.0, 1.0);
  stress = clamp(stress - 0.02 * dt_seconds, 0.0, 1.0);
}

NpcPsychologyProfile::NpcPsychologyProfile(world_sim::EntityId seed_id) {
  // Deterministic per-NPC behavioral variation without runtime RNG
  habits_.punctuality = 0.35 + 0.60 * hash_unit(seed_id * 17ULL + 1ULL);
  habits_.risk_tolerance = 0.15 + 0.75 * hash_unit(seed_id * 31ULL + 3ULL);
  habits_.social_battery = 0.25 + 0.70 * hash_unit(seed_id * 47ULL + 7ULL);
  habits_.weather_sensitivity = 0.20 + 0.75 * hash_unit(seed_id * 59ULL + 11ULL);
  const auto cat_idx = static_cast<std::uint8_t>((seed_id % 4) + 1);
  habits_.preferred_category = static_cast<world_sim::VenueCategory>(cat_idx);
}

void NpcPsychologyProfile::record_experience(const ExperienceEntry& entry) {
  if (!std::isfinite(entry.sentiment)) return;
  ExperienceEntry clamped = entry;
  clamped.sentiment = clamp(entry.sentiment, -1.0, 1.0);
  while (experiences_.size() >= max_experiences_) {
    experiences_.erase(experiences_.begin());
  }
  experiences_.push_back(clamped);
}

void NpcPsychologyProfile::apply_emotional_stimulus(double delta_valence,
                                                    double delta_fear,
                                                    double delta_anger,
                                                    double delta_stress) noexcept {
  emotions_.valence = clamp(emotions_.valence + delta_valence, -1.0, 1.0);
  emotions_.fear = clamp(emotions_.fear + delta_fear, 0.0, 1.0);
  emotions_.anger = clamp(emotions_.anger + delta_anger, 0.0, 1.0);
  emotions_.stress = clamp(emotions_.stress + delta_stress, 0.0, 1.0);
  emotions_.arousal = clamp(
      std::max(emotions_.arousal, 0.5 * emotions_.fear + 0.4 * emotions_.anger),
      0.0, 1.0);
}

void NpcPsychologyProfile::step(double dt_seconds,
                                const EnvironmentalCondition& env) noexcept {
  emotions_.step_decay(dt_seconds);
  if (env.storm_severity > 0.5 || env.flood_depth_m > 0.15) {
    const double sev = std::max(env.storm_severity, env.flood_depth_m);
    emotions_.stress = clamp(
        emotions_.stress + 0.04 * sev * habits_.weather_sensitivity * dt_seconds,
        0.0, 1.0);
  }
}

double NpcPsychologyProfile::venue_affinity(
    world_sim::VenueId venue_id) const noexcept {
  if (venue_id == world_sim::kInvalidVenueId) return 0.0;
  double sum = 0.0;
  std::uint32_t count = 0;
  for (const auto& e : experiences_) {
    if (e.venue_id == venue_id) {
      sum += e.sentiment;
      ++count;
    }
  }
  return (count > 0) ? (sum / static_cast<double>(count)) : 0.0;
}

PsychologyDecision NpcPsychologyProfile::evaluate(
    const EnvironmentalCondition& env,
    world_sim::VenueId candidate_venue) const noexcept {
  PsychologyDecision out{};
  out.venue_bias = venue_affinity(candidate_venue);

  if (env.flood_depth_m > 0.25 || emotions_.fear > 0.75) {
    out.action = ContextualAction::FleeHazard;
    out.speed_multiplier = 1.6;
    return out;
  }

  if (env.rain_intensity * habits_.weather_sensitivity > 0.45 ||
      env.storm_severity > 0.55 || env.temperature_c < -5.0 ||
      env.temperature_c > 42.0) {
    out.action = ContextualAction::SeekShelter;
    out.speed_multiplier = 1.35;
    return out;
  }

  if (out.venue_bias < -0.45 && habits_.risk_tolerance < 0.7) {
    out.action = ContextualAction::AvoidArea;
    out.speed_multiplier = 1.1;
    return out;
  }

  if (emotions_.valence > 0.35 && habits_.social_battery > 0.5) {
    out.action = ContextualAction::EngageSocial;
    out.speed_multiplier = 1.0;
    return out;
  }

  out.action = ContextualAction::FollowRoutine;
  out.speed_multiplier = 1.0;
  return out;
}

}  // namespace saige::psychology
