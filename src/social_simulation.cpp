#include "saige/social_simulation.hpp"

#include <algorithm>
#include <cmath>

namespace saige::social {

namespace {

[[nodiscard]] float clampf(float v, float lo, float hi) noexcept {
  if (!std::isfinite(v) || v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

[[nodiscard]] std::pair<world_sim::EntityId, world_sim::EntityId> canonical_pair(
    world_sim::EntityId x, world_sim::EntityId y) noexcept {
  return (x < y) ? std::pair{x, y} : std::pair{y, x};
}

}  // namespace

bool SocialGathering::is_active(std::uint16_t minute_of_day) const noexcept {
  if (start_minute == end_minute) return true;
  if (start_minute < end_minute) {
    return minute_of_day >= start_minute && minute_of_day < end_minute;
  }
  return minute_of_day >= start_minute || minute_of_day < end_minute;
}

bool SocialNetwork::set_relationship(const SocialEdge& edge) {
  if (edge.a == world_sim::kInvalidEntityId ||
      edge.b == world_sim::kInvalidEntityId || edge.a == edge.b) {
    return false;
  }
  const auto [ca, cb] = canonical_pair(edge.a, edge.b);
  SocialEdge norm = edge;
  norm.a = ca;
  norm.b = cb;
  if (auto* existing = find_relationship_mut(ca, cb)) {
    *existing = norm;
    return true;
  }
  edges_.push_back(norm);
  std::sort(edges_.begin(), edges_.end(), [](const SocialEdge& x, const SocialEdge& y) {
    if (x.a != y.a) return x.a < y.a;
    return x.b < y.b;
  });
  return true;
}

const SocialEdge* SocialNetwork::find_relationship(
    world_sim::EntityId a, world_sim::EntityId b) const noexcept {
  const auto [ca, cb] = canonical_pair(a, b);
  for (const auto& e : edges_) {
    if (e.a == ca && e.b == cb) return &e;
  }
  return nullptr;
}

SocialEdge* SocialNetwork::find_relationship_mut(
    world_sim::EntityId a, world_sim::EntityId b) noexcept {
  const auto [ca, cb] = canonical_pair(a, b);
  for (auto& e : edges_) {
    if (e.a == ca && e.b == cb) return &e;
  }
  return nullptr;
}

SocialEncounterDecision SocialNetwork::evaluate_encounter(
    const world_sim::NpcAgent& actor, const world_sim::NpcAgent& target,
    const psychology::NpcPsychologyProfile& actor_psych,
    const psychology::EnvironmentalCondition& env,
    double current_time_sec,
    double max_encounter_distance_m) const noexcept {
  SocialEncounterDecision out{};
  if (actor.id == target.id) return out;
  if (world_sim::WorldPosition::distance(actor.position, target.position) >
      max_encounter_distance_m) {
    return out;
  }

  const SocialEdge* edge = find_relationship(actor.id, target.id);
  const float aff = edge ? edge->affinity : 0.0f;
  const float trust = edge ? edge->trust : 0.0f;
  const float fam = edge ? edge->familiarity : 0.0f;

  // Cooldown to avoid spamming interactions every tick
  if (edge != nullptr && (current_time_sec - edge->last_interaction_sec) < 10.0) {
    return out;
  }

  // Confront or Avoid negative relationships
  if (aff < -0.45f || trust < -0.40f) {
    if (actor_psych.emotions().anger > 0.55 || actor.personality.aggression > 0.65) {
      out.action = SocialAction::Confront;
      out.affinity_delta = -0.10;
      out.trust_delta = -0.10;
      out.emotion_valence_delta = -0.20;
    } else {
      out.action = SocialAction::Avoid;
      out.affinity_delta = -0.02;
      out.trust_delta = -0.02;
      out.emotion_valence_delta = -0.05;
    }
    return out;
  }

  // Help distressed ally or family member
  if ((aff > 0.30f || (edge && edge->kind == persistent_world::RelationshipKind::Family)) &&
      target.drives.safety < 0.55) {
    out.action = SocialAction::HelpAlly;
    out.affinity_delta = 0.12;
    out.trust_delta = 0.15;
    out.emotion_valence_delta = 0.15;
    return out;
  }

  // Storm/emergency reduces casual chat to brief wave
  if (env.storm_severity > 0.6 || actor_psych.emotions().fear > 0.5) {
    if (fam > 0.25f) {
      out.action = SocialAction::WaveGreeting;
      out.affinity_delta = 0.02;
      out.trust_delta = 0.01;
      out.emotion_valence_delta = 0.05;
    }
    return out;
  }

  if (fam > 0.45f && aff > 0.35f && actor_psych.habits().social_battery > 0.4) {
    out.action = SocialAction::DeepConversation;
    out.affinity_delta = 0.08;
    out.trust_delta = 0.06;
    out.emotion_valence_delta = 0.15;
  } else if (actor.personality.sociability > 0.35 || fam > 0.15f) {
    out.action = SocialAction::FriendlyChat;
    out.affinity_delta = 0.06;
    out.trust_delta = 0.04;
    out.emotion_valence_delta = 0.10;
  } else {
    out.action = SocialAction::WaveGreeting;
    out.affinity_delta = 0.03;
    out.trust_delta = 0.02;
    out.emotion_valence_delta = 0.04;
  }
  return out;
}

bool SocialNetwork::apply_encounter(
    world_sim::NpcAgent& actor, world_sim::NpcAgent& target,
    psychology::NpcPsychologyProfile& actor_psych,
    const psychology::EnvironmentalCondition& env, double current_time_sec,
    double max_encounter_distance_m) {
  const auto dec = evaluate_encounter(actor, target, actor_psych, env,
                                      current_time_sec, max_encounter_distance_m);
  if (dec.action == SocialAction::None) return false;

  SocialEdge* edge = find_relationship_mut(actor.id, target.id);
  if (edge == nullptr) {
    SocialEdge created{};
    const auto [ca, cb] = canonical_pair(actor.id, target.id);
    created.a = ca;
    created.b = cb;
    set_relationship(created);
    edge = find_relationship_mut(actor.id, target.id);
  }
  if (edge == nullptr) return false;

  edge->familiarity = clampf(edge->familiarity + 0.10f, 0.0f, 1.0f);
  edge->affinity =
      clampf(edge->affinity + static_cast<float>(dec.affinity_delta), -1.0f, 1.0f);
  edge->trust =
      clampf(edge->trust + static_cast<float>(dec.trust_delta), -1.0f, 1.0f);
  ++edge->interaction_count;
  edge->last_interaction_sec = current_time_sec;

  // Natural relationship progression from repeated interactions
  if (edge->kind == persistent_world::RelationshipKind::Acquaintance &&
      edge->affinity >= 0.45f && edge->familiarity >= 0.40f) {
    edge->kind = persistent_world::RelationshipKind::Friend;
  } else if (edge->affinity <= -0.50f) {
    edge->kind = persistent_world::RelationshipKind::Rival;
  }

  actor_psych.apply_emotional_stimulus(dec.emotion_valence_delta, 0.0,
                                       dec.action == SocialAction::Confront ? 0.15 : 0.0,
                                       0.0);
  actor_psych.record_experience(
      {world_sim::kInvalidVenueId, target.id, dec.affinity_delta * 2.0, current_time_sec});
  actor.drives.social = std::min(1.0, actor.drives.social + 0.15);
  return true;
}

bool SocialNetwork::add_gathering(const SocialGathering& gathering) {
  if (gathering.id == 0) return false;
  for (const auto& g : gatherings_) {
    if (g.id == gathering.id) return false;
  }
  gatherings_.push_back(gathering);
  return true;
}

world_sim::WorldPosition SocialNetwork::compute_group_formation_slot(
    const world_sim::WorldPosition& leader_pos, std::size_t member_idx,
    std::size_t group_size, double chunk_size_m) const noexcept {
  if (group_size <= 1) return leader_pos;
  constexpr double kTwoPi = 6.283185307179586;
  const double angle =
      (static_cast<double>(member_idx) / static_cast<double>(group_size)) * kTwoPi;
  const double radius_m = 1.8;
  const world_sim::Vec3d w = leader_pos.to_world_double(chunk_size_m);
  return world_sim::WorldPosition::from_world_double(
      {w.x + std::cos(angle) * radius_m, w.y + std::sin(angle) * radius_m, w.z},
      chunk_size_m);
}

std::uint32_t SocialNetwork::apply_gathering_schedules(
    std::vector<world_sim::NpcAgent>& npcs, std::uint16_t minute_of_day) const {
  std::uint32_t scheduled = 0;
  for (const auto& g : gatherings_) {
    if (!g.is_active(minute_of_day)) continue;
    const std::size_t n = g.participants.size();
    for (std::size_t idx = 0; idx < n; ++idx) {
      const world_sim::EntityId pid = g.participants[idx];
      for (auto& npc : npcs) {
        if (npc.id == pid) {
          // Behavioral priority arbitration: never override urgent/emergency intents
          if (npc.current_nav_intent.urgent ||
              npc.current_nav_intent.intent == world_sim::NpcIntent::Flee ||
              npc.current_nav_intent.intent == world_sim::NpcIntent::Assist ||
              npc.current_nav_intent.intent == world_sim::NpcIntent::Investigate) {
            break;
          }
          npc.current_nav_intent.intent = world_sim::NpcIntent::Socialize;
          npc.current_nav_intent.has_destination = true;
          npc.current_nav_intent.target_venue = g.venue_id;
          npc.current_nav_intent.target_position =
              compute_group_formation_slot(g.center, idx, n);
          ++scheduled;
          break;
        }
      }
    }
  }
  return scheduled;
}

void SocialNetwork::sync_to_persistent_store(
    persistent_world::PersistentWorldStore& store) const {
  for (const auto& e : edges_) {
    store.upsert_relationship({e.a, e.b, e.kind, e.affinity, e.familiarity,
                               e.trust, e.interaction_count,
                               e.last_interaction_sec});
  }
}

void SocialNetwork::load_from_persistent_store(
    const persistent_world::PersistentWorldStore& store) {
  edges_.clear();
  for (const auto& r : store.relationships()) {
    set_relationship({r.from_id, r.to_id, r.kind, r.affinity, r.familiarity,
                      r.trust, r.interaction_count, r.last_interaction_sec});
  }
}

}  // namespace saige::social
