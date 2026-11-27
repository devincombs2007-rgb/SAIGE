#include "saige/npc_simulation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace saige::world_sim {

namespace {

[[nodiscard]] double clamp01(double v) noexcept {
  if (!std::isfinite(v) || v < 0.0) return 0.0;
  if (v > 1.0) return 1.0;
  return v;
}

[[nodiscard]] bool is_finite_vec3f(const Vec3f& v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

[[nodiscard]] bool is_finite_pos(const WorldPosition& p) noexcept {
  return is_finite_vec3f(p.local);
}

[[nodiscard]] std::int32_t clamp_chunk_index(double v) noexcept {
  constexpr double kMinIdx =
      static_cast<double>(std::numeric_limits<std::int32_t>::min());
  constexpr double kMaxIdx =
      static_cast<double>(std::numeric_limits<std::int32_t>::max());
  if (!std::isfinite(v)) return 0;
  if (v <= kMinIdx) return std::numeric_limits<std::int32_t>::min();
  if (v >= kMaxIdx) return std::numeric_limits<std::int32_t>::max();
  return static_cast<std::int32_t>(v);
}

// Cast to double BEFORE subtracting to prevent signed int32_t overflow (UB).
[[nodiscard]] Vec3d delta_world(const WorldPosition& from,
                                const WorldPosition& to,
                                double chunk_size_m) noexcept {
  const double dcx =
      static_cast<double>(to.cell.x) - static_cast<double>(from.cell.x);
  const double dcy =
      static_cast<double>(to.cell.y) - static_cast<double>(from.cell.y);
  return {
      dcx * chunk_size_m +
          (static_cast<double>(to.local.x) - static_cast<double>(from.local.x)),
      dcy * chunk_size_m +
          (static_cast<double>(to.local.y) - static_cast<double>(from.local.y)),
      static_cast<double>(to.local.z) - static_cast<double>(from.local.z),
  };
}

}  // namespace

Vec3d WorldPosition::to_world_double(double chunk_size_m) const noexcept {
  return {
      static_cast<double>(cell.x) * chunk_size_m + static_cast<double>(local.x),
      static_cast<double>(cell.y) * chunk_size_m + static_cast<double>(local.y),
      static_cast<double>(local.z),
  };
}

WorldPosition WorldPosition::from_world_double(const Vec3d& pos,
                                               double chunk_size_m) noexcept {
  const double safe_chunk = (std::isfinite(chunk_size_m) && chunk_size_m > 0.0)
                                ? chunk_size_m
                                : kDefaultChunkSizeM;
  const double cx = std::floor(pos.x / safe_chunk);
  const double cy = std::floor(pos.y / safe_chunk);
  WorldPosition out{};
  out.cell.x = clamp_chunk_index(cx);
  out.cell.y = clamp_chunk_index(cy);
  out.local.x = static_cast<float>(pos.x - static_cast<double>(out.cell.x) * safe_chunk);
  out.local.y = static_cast<float>(pos.y - static_cast<double>(out.cell.y) * safe_chunk);
  out.local.z = static_cast<float>(pos.z);
  return out;
}

double WorldPosition::distance(const WorldPosition& a, const WorldPosition& b,
                               double chunk_size_m) noexcept {
  const Vec3d d = delta_world(a, b, chunk_size_m);
  return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

void WorldClock::advance(double dt_seconds) noexcept {
  if (!std::isfinite(dt_seconds) || dt_seconds <= 0.0) {
    return;
  }
  const double safe_scale =
      (std::isfinite(time_scale) && time_scale >= 0.0) ? time_scale : 0.0;
  ++total_ticks;
  total_seconds += dt_seconds;
  const double delta_minutes = (dt_seconds * safe_scale) / 60.0;
  minute_of_day += delta_minutes;
  while (minute_of_day >= 1440.0) {
    minute_of_day -= 1440.0;
    ++day_index;
  }
}

std::uint16_t WorldClock::current_minute_int() const noexcept {
  const double floored = std::floor(minute_of_day);
  if (!std::isfinite(floored) || floored < 0.0) return 0;
  if (floored >= 1440.0) return 1439;
  return static_cast<std::uint16_t>(floored);
}

bool Venue::is_open(std::uint16_t minute_of_day) const noexcept {
  if (open_minute == close_minute) {
    return true;
  }
  if (open_minute < close_minute) {
    return minute_of_day >= open_minute && minute_of_day < close_minute;
  }
  return minute_of_day >= open_minute || minute_of_day < close_minute;
}

bool Venue::has_capacity() const noexcept {
  return current_occupants < capacity;
}

bool ScheduleBlock::contains_minute(std::uint16_t minute_of_day) const noexcept {
  if (start_minute == end_minute) {
    return true;
  }
  if (start_minute < end_minute) {
    return minute_of_day >= start_minute && minute_of_day < end_minute;
  }
  return minute_of_day >= start_minute || minute_of_day < end_minute;
}

WorldSimulation::WorldSimulation(SimBudgetConfig config) : config_(config) {
  if (!std::isfinite(config_.chunk_size_m) || config_.chunk_size_m <= 0.0) {
    config_.chunk_size_m = kDefaultChunkSizeM;
  }
  if (!std::isfinite(config_.active_radius_m) || config_.active_radius_m < 0.0) {
    config_.active_radius_m = 60.0;
  }
  if (!std::isfinite(config_.background_radius_m) ||
      config_.background_radius_m < config_.active_radius_m) {
    config_.background_radius_m = config_.active_radius_m + 50.0;
  }
  if (!std::isfinite(config_.lod_hysteresis_m) || config_.lod_hysteresis_m < 0.0) {
    config_.lod_hysteresis_m = 0.0;
  }
}

bool WorldSimulation::register_venue(const Venue& venue) {
  if (venue.id == kInvalidVenueId || !is_finite_pos(venue.anchor_position)) {
    return false;
  }
  for (const auto& existing : venues_) {
    if (existing.id == venue.id) {
      return false;
    }
  }
  Venue sanitized = venue;
  sanitized.current_occupants = 0;
  venues_.push_back(sanitized);
  std::sort(venues_.begin(), venues_.end(),
            [](const Venue& a, const Venue& b) { return a.id < b.id; });
  return true;
}

const Venue* WorldSimulation::find_venue(VenueId id) const noexcept {
  for (const auto& v : venues_) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

Venue* WorldSimulation::find_venue_mut(VenueId id) noexcept {
  for (auto& v : venues_) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

bool WorldSimulation::register_npc(NpcAgent agent) {
  if (agent.id == kInvalidEntityId || !is_finite_pos(agent.position)) {
    return false;
  }
  for (const auto& existing : npcs_) {
    if (existing.id == agent.id) {
      return false;
    }
  }
  agent.reserved_venue = kInvalidVenueId;
  if (!std::isfinite(agent.vision_range_m) || agent.vision_range_m < 0.0) {
    agent.vision_range_m = 25.0;
  }
  if (!std::isfinite(agent.hearing_range_m) || agent.hearing_range_m < 0.0) {
    agent.hearing_range_m = 35.0;
  }
  npcs_.push_back(std::move(agent));
  std::sort(npcs_.begin(), npcs_.end(),
            [](const NpcAgent& a, const NpcAgent& b) { return a.id < b.id; });
  return true;
}

bool WorldSimulation::remove_npc(EntityId id) {
  const auto it = std::find_if(npcs_.begin(), npcs_.end(),
                               [id](const NpcAgent& a) { return a.id == id; });
  if (it == npcs_.end()) {
    return false;
  }
  release_venue_reservation(*it);
  npcs_.erase(it);
  if (!npcs_.empty()) {
    active_cursor_ %= npcs_.size();
    bg_cursor_ %= npcs_.size();
    dormant_cursor_ %= npcs_.size();
  } else {
    active_cursor_ = 0;
    bg_cursor_ = 0;
    dormant_cursor_ = 0;
  }
  return true;
}

NpcAgent* WorldSimulation::find_npc(EntityId id) noexcept {
  for (auto& a : npcs_) {
    if (a.id == id) return &a;
  }
  return nullptr;
}

const NpcAgent* WorldSimulation::find_npc(EntityId id) const noexcept {
  for (const auto& a : npcs_) {
    if (a.id == id) return &a;
  }
  return nullptr;
}

void WorldSimulation::set_chunk_loaded(ChunkCoord chunk, bool loaded) {
  const auto it = std::find(unloaded_chunks_.begin(), unloaded_chunks_.end(), chunk);
  if (loaded) {
    if (it != unloaded_chunks_.end()) {
      unloaded_chunks_.erase(it);
    }
  } else {
    if (it == unloaded_chunks_.end()) {
      unloaded_chunks_.push_back(chunk);
    }
    for (auto& npc : npcs_) {
      if (npc.position.cell == chunk) {
        npc.lod = SimLodTier::Dormant;
        npc.short_term_memory.clear();
      }
    }
  }
}

bool WorldSimulation::is_chunk_loaded(ChunkCoord chunk) const noexcept {
  return std::find(unloaded_chunks_.begin(), unloaded_chunks_.end(), chunk) ==
         unloaded_chunks_.end();
}

void WorldSimulation::assign_occupation_schedule(NpcAgent& npc,
                                                 OccupationKind occupation,
                                                 VenueId work_venue,
                                                 VenueId home_venue) {
  npc.occupation = occupation;
  npc.persistent_memory.home_venue = home_venue;
  npc.persistent_memory.work_venue = work_venue;
  npc.schedule.clear();
  switch (occupation) {
    case OccupationKind::Shopkeeper:
    case OccupationKind::OfficeWorker:
      npc.schedule.push_back({480, 1020, work_venue, NpcIntent::Work});
      npc.schedule.push_back({1020, 1260, home_venue, NpcIntent::Rest});
      npc.schedule.push_back({1260, 480, home_venue, NpcIntent::Rest});
      break;
    case OccupationKind::NightGuard:
      npc.schedule.push_back({1320, 360, work_venue, NpcIntent::Work});
      npc.schedule.push_back({360, 1320, home_venue, NpcIntent::Rest});
      break;
    case OccupationKind::Paramedic:
      npc.schedule.push_back({0, 1440, work_venue, NpcIntent::Work});
      break;
    case OccupationKind::Unemployed:
      npc.schedule.push_back({0, 1440, home_venue, NpcIntent::Rest});
      break;
  }
}

bool WorldSimulation::report_traffic_accident(std::uint32_t event_id,
                                              const WorldPosition& pos,
                                              double duration_sec) {
  WorldEventRecord rec{};
  rec.id = event_id;
  rec.kind = SensoryKind::EmergencyAlert;
  rec.position = pos;
  rec.radius_m = 45.0;
  rec.intensity = 0.9;
  rec.expires_at_sec = clock_.total_seconds + std::max(1.0, duration_sec);
  return register_world_event(rec);
}

bool WorldSimulation::register_world_event(const WorldEventRecord& event) {
  if (event.id == 0 || !is_finite_pos(event.position) ||
      !std::isfinite(event.radius_m) || event.radius_m <= 0.0 ||
      !std::isfinite(event.intensity) || event.intensity <= 0.0) {
    return false;
  }
  for (auto& existing : world_events_) {
    if (existing.id == event.id) {
      existing = event;
      return true;
    }
  }
  world_events_.push_back(event);
  return true;
}

std::uint32_t WorldSimulation::emit_sensory_event(const SensoryEvent& event) {
  if (!is_finite_pos(event.position) || !std::isfinite(event.intensity) ||
      event.intensity <= 0.0) {
    return 0;
  }
  std::uint32_t notified = 0;
  for (auto& npc : npcs_) {
    if (npc.id == event.source_id || npc.lod == SimLodTier::Dormant) {
      continue;
    }
    if (!is_chunk_loaded(npc.position.cell)) {
      continue;
    }
    const double max_range = (event.kind == SensoryKind::Visual)
                                 ? npc.vision_range_m
                                 : npc.hearing_range_m;
    const double dist = WorldPosition::distance(npc.position, event.position,
                                                config_.chunk_size_m);
    if (dist > max_range) {
      continue;
    }

    SensoryEvent recorded = event;
    recorded.timestamp_sec = clock_.total_seconds;
    recorded.ttl_sec =
        (std::isfinite(event.ttl_sec) && event.ttl_sec > 0.0) ? event.ttl_sec : 15.0;
    const double falloff = 1.0 - (dist / std::max(1.0, max_range));
    recorded.intensity = clamp01(event.intensity * (0.35 + 0.65 * falloff));

    if (npc.max_short_term_memories > 0) {
      while (npc.short_term_memory.size() >= npc.max_short_term_memories) {
        npc.short_term_memory.erase(npc.short_term_memory.begin());
      }
      npc.short_term_memory.push_back(recorded);
    }

    if (event.kind == SensoryKind::PhysicalDisturbance ||
        event.kind == SensoryKind::EmergencyAlert) {
      npc.persistent_memory.last_threat_source = event.source_id;
      npc.persistent_memory.threat_memory_strength =
          std::max(npc.persistent_memory.threat_memory_strength, recorded.intensity);
      ++npc.persistent_memory.disturbance_count;
      npc.drives.safety = clamp01(npc.drives.safety - 0.55 * recorded.intensity);
    } else if (event.kind == SensoryKind::SocialGreeting) {
      npc.drives.social = clamp01(npc.drives.social + 0.15 * recorded.intensity);
    }
    ++notified;
  }
  return notified;
}

SimLodTier WorldSimulation::compute_lod_with_hysteresis(
    SimLodTier current, double distance_m) const noexcept {
  const double h = config_.lod_hysteresis_m;
  switch (current) {
    case SimLodTier::Active:
      if (distance_m > config_.background_radius_m + h) return SimLodTier::Dormant;
      if (distance_m > config_.active_radius_m + h) return SimLodTier::Background;
      return SimLodTier::Active;
    case SimLodTier::Background:
      if (distance_m <= config_.active_radius_m - h) return SimLodTier::Active;
      if (distance_m > config_.background_radius_m + h) return SimLodTier::Dormant;
      return SimLodTier::Background;
    case SimLodTier::Dormant:
      if (distance_m <= config_.active_radius_m - h) return SimLodTier::Active;
      if (distance_m <= config_.background_radius_m - h) return SimLodTier::Background;
      return SimLodTier::Dormant;
  }
  return SimLodTier::Dormant;
}

void WorldSimulation::update_drives_and_memory(NpcAgent& npc,
                                               double dt_seconds) const {
  const double dt_min = (dt_seconds * clock_.time_scale) / 60.0;
  npc.drives.hunger = clamp01(npc.drives.hunger + 0.0015 * dt_min);
  if (npc.current_nav_intent.intent == NpcIntent::Rest) {
    npc.drives.energy = clamp01(npc.drives.energy + 0.004 * dt_min);
  } else {
    npc.drives.energy = clamp01(npc.drives.energy - 0.001 * dt_min);
  }
  if (npc.current_nav_intent.intent == NpcIntent::Socialize) {
    npc.drives.social = clamp01(npc.drives.social + 0.003 * dt_min);
  } else {
    npc.drives.social = clamp01(npc.drives.social - 0.0005 * dt_min);
  }
  npc.drives.safety = clamp01(npc.drives.safety + 0.02 * dt_seconds);
  npc.persistent_memory.threat_memory_strength =
      clamp01(npc.persistent_memory.threat_memory_strength - 0.005 * dt_seconds);

  const double now = clock_.total_seconds;
  npc.reacted_world_events.erase(
      std::remove_if(npc.reacted_world_events.begin(),
                     npc.reacted_world_events.end(),
                     [now](const NpcAgent::ReactedWorldEvent& r) {
                       return now >= r.expires_at_sec;
                     }),
      npc.reacted_world_events.end());
  npc.short_term_memory.erase(
      std::remove_if(npc.short_term_memory.begin(), npc.short_term_memory.end(),
                     [now](const SensoryEvent& e) {
                       return (now - e.timestamp_sec) > e.ttl_sec;
                     }),
      npc.short_term_memory.end());
}

void WorldSimulation::release_venue_reservation(NpcAgent& npc) noexcept {
  if (npc.reserved_venue == kInvalidVenueId) return;
  Venue* v = find_venue_mut(npc.reserved_venue);
  if (v != nullptr && v->current_occupants > 0) {
    --v->current_occupants;
  }
  npc.reserved_venue = kInvalidVenueId;
}

bool WorldSimulation::try_reserve_venue(NpcAgent& npc,
                                        VenueId venue_id) noexcept {
  if (venue_id == kInvalidVenueId) {
    release_venue_reservation(npc);
    return true;
  }
  if (npc.reserved_venue == venue_id) return true;
  Venue* target = find_venue_mut(venue_id);
  if (target == nullptr || !target->has_capacity()) return false;
  release_venue_reservation(npc);
  ++target->current_occupants;
  npc.reserved_venue = venue_id;
  return true;
}

bool WorldSimulation::evaluate_npc_decision(NpcAgent& npc,
                                            SimStepMetrics& metrics) {
  const NpcIntent previous_intent = npc.current_nav_intent.intent;
  const std::uint16_t minute = clock_.current_minute_int();

  double max_threat = 0.0;
  WorldPosition threat_pos{};
  bool has_threat = false;
  for (const auto& mem : npc.short_term_memory) {
    if (mem.kind == SensoryKind::PhysicalDisturbance ||
        mem.kind == SensoryKind::EmergencyAlert ||
        mem.kind == SensoryKind::Auditory) {
      if (mem.intensity > max_threat) {
        max_threat = mem.intensity;
        threat_pos = mem.position;
        has_threat = true;
      }
    }
  }

  double best_score = 0.05;
  NavigationIntent best_nav{};
  best_nav.intent = NpcIntent::Idle;

  if (has_threat && max_threat > 0.25 &&
      npc.occupation == OccupationKind::Paramedic) {
    best_score = 1.8;
    best_nav.intent = NpcIntent::Assist;
    best_nav.urgent = true;
    best_nav.desired_speed = 4.5;
    best_nav.has_destination = true;
    best_nav.target_position = threat_pos;
  } else if (has_threat && max_threat > 0.25) {
    const double flee_score =
        max_threat * (1.2 - 0.7 * npc.personality.bravery) +
        (1.0 - npc.drives.safety) * 0.6;
    const double investigate_score =
        max_threat * (0.4 + 0.6 * npc.personality.curiosity) *
        npc.personality.bravery;

    if (flee_score >= investigate_score && flee_score > best_score) {
      best_score = flee_score;
      best_nav.intent = NpcIntent::Flee;
      best_nav.urgent = true;
      best_nav.desired_speed = 4.2;
      // Horizontal flee vector on SAIGE's X/Y plane (Z-up preserved)
      const Vec3d d = delta_world(threat_pos, npc.position, config_.chunk_size_m);
      const double len = std::sqrt(d.x * d.x + d.y * d.y);
      const double nx = (len > 1e-4) ? (d.x / len) : 1.0;
      const double ny = (len > 1e-4) ? (d.y / len) : 0.0;
      const Vec3d cur_w = npc.position.to_world_double(config_.chunk_size_m);
      best_nav.has_destination = true;
      best_nav.target_position = WorldPosition::from_world_double(
          {cur_w.x + nx * 30.0, cur_w.y + ny * 30.0, cur_w.z},
          config_.chunk_size_m);
    } else if (investigate_score > best_score) {
      best_score = investigate_score;
      best_nav.intent = NpcIntent::Investigate;
      best_nav.urgent = false;
      best_nav.desired_speed = 2.0;
      best_nav.has_destination = true;
      best_nav.target_position = threat_pos;
    }
  }

  for (const auto& block : npc.schedule) {
    if (!block.contains_minute(minute)) continue;
    const Venue* venue = find_venue(block.target_venue);
    if (venue != nullptr) {
      if (!venue->is_open(minute)) continue;
      if (npc.reserved_venue != venue->id && !venue->has_capacity()) {
        ++metrics.venue_capacity_denials;
        continue;
      }
    }
    double sched_score = 0.45 + 0.45 * npc.personality.conscientiousness;
    if (block.intent == NpcIntent::Rest) {
      sched_score += (1.0 - npc.drives.energy) * 0.5;
    } else if (block.intent == NpcIntent::Shop) {
      sched_score += npc.drives.hunger * 0.4;
    } else if (block.intent == NpcIntent::Socialize) {
      sched_score += (1.0 - npc.drives.social) * 0.35 +
                     npc.personality.sociability * 0.25;
    }

    if (sched_score > best_score) {
      best_score = sched_score;
      best_nav.intent = block.intent;
      best_nav.urgent = false;
      best_nav.desired_speed = 1.45;
      best_nav.target_venue = block.target_venue;
      if (venue != nullptr) {
        best_nav.has_destination = true;
        best_nav.target_position = venue->anchor_position;
      } else {
        best_nav.has_destination = false;
      }
    }
  }

  if (npc.drives.energy < 0.20 &&
      npc.persistent_memory.home_venue != kInvalidVenueId) {
    const Venue* home = find_venue(npc.persistent_memory.home_venue);
    const double rest_score = 0.75 + (0.20 - npc.drives.energy);
    if (home != nullptr &&
        (npc.reserved_venue == home->id || home->has_capacity()) &&
        rest_score > best_score) {
      best_score = rest_score;
      best_nav.intent = NpcIntent::Rest;
      best_nav.urgent = false;
      best_nav.desired_speed = 1.35;
      best_nav.has_destination = true;
      best_nav.target_position = home->anchor_position;
      best_nav.target_venue = home->id;
    }
  }

  if (best_nav.target_venue != kInvalidVenueId) {
    if (!try_reserve_venue(npc, best_nav.target_venue)) {
      ++metrics.venue_capacity_denials;
      best_nav = NavigationIntent{};
      best_nav.intent = NpcIntent::Idle;
    }
  } else {
    release_venue_reservation(npc);
  }

  npc.current_nav_intent = best_nav;
  npc.last_eval_time_sec = clock_.total_seconds;
  ++npc.evaluation_count;
  return npc.current_nav_intent.intent != previous_intent;
}

SimStepMetrics WorldSimulation::step(double dt_seconds,
                                     const WorldPosition& focus_position) {
  SimStepMetrics metrics{};
  metrics.total_npcs = static_cast<std::uint32_t>(npcs_.size());
  if (!std::isfinite(dt_seconds) || dt_seconds <= 0.0 ||
      !is_finite_pos(focus_position)) {
    return metrics;
  }

  clock_.advance(dt_seconds);


  // Expire elapsed persistent world events and notify nearby non-Dormant NPCs
  const double now_sec = clock_.total_seconds;
  world_events_.erase(
      std::remove_if(world_events_.begin(), world_events_.end(),
                     [now_sec](const WorldEventRecord& we) {
                       return now_sec >= we.expires_at_sec;
                     }),
      world_events_.end());
  for (const auto& we : world_events_) {
    for (auto& npc : npcs_) {
      if (npc.lod == SimLodTier::Dormant || !is_chunk_loaded(npc.position.cell)) {
        continue;
      }
      if (WorldPosition::distance(npc.position, we.position, config_.chunk_size_m) <=
          we.radius_m) {
        auto it = std::lower_bound(
            npc.reacted_world_events.begin(), npc.reacted_world_events.end(),
            we.id, [](const NpcAgent::ReactedWorldEvent& r, std::uint32_t id) {
              return r.event_id < id;
            });
        const bool already_reacted =
            (it != npc.reacted_world_events.end() && it->event_id == we.id);
        if (!already_reacted) {
          npc.reacted_world_events.insert(it, {we.id, we.expires_at_sec});
          ++npc.world_event_reaction_count;
          SensoryEvent ev{};
          ev.kind = we.kind;
          ev.source_id = we.id;
          ev.position = we.position;
          ev.intensity = we.intensity;
          ev.timestamp_sec = now_sec;
          ev.ttl_sec = 5.0;
          if (npc.max_short_term_memories > 0) {
            while (npc.short_term_memory.size() >= npc.max_short_term_memories) {
              npc.short_term_memory.erase(npc.short_term_memory.begin());
            }
            npc.short_term_memory.push_back(ev);
          }
          npc.drives.safety = clamp01(npc.drives.safety - 0.25 * we.intensity);
        }
      }
    }
  }

  std::vector<std::size_t> active_indices;
  std::vector<std::size_t> bg_indices;
  std::vector<std::size_t> dormant_indices;
  active_indices.reserve(npcs_.size());
  bg_indices.reserve(npcs_.size());
  dormant_indices.reserve(npcs_.size());

  for (std::size_t i = 0; i < npcs_.size(); ++i) {
    auto& npc = npcs_[i];
    if (!is_chunk_loaded(npc.position.cell)) {
      npc.lod = SimLodTier::Dormant;
    } else {
      const double dist = WorldPosition::distance(npc.position, focus_position,
                                                  config_.chunk_size_m);
      npc.lod = compute_lod_with_hysteresis(npc.lod, dist);
    }
    update_drives_and_memory(npc, dt_seconds);

    switch (npc.lod) {
      case SimLodTier::Active:
        ++metrics.active_count;
        active_indices.push_back(i);
        break;
      case SimLodTier::Background:
        ++metrics.background_count;
        bg_indices.push_back(i);
        break;
      case SimLodTier::Dormant:
        ++metrics.dormant_count;
        dormant_indices.push_back(i);
        break;
    }
  }

  if (!active_indices.empty() && config_.max_active_evals_per_tick > 0) {
    const std::size_t count = active_indices.size();
    const std::uint32_t limit = std::min<std::uint32_t>(
        config_.max_active_evals_per_tick, static_cast<std::uint32_t>(count));
    for (std::uint32_t k = 0; k < limit; ++k) {
      const std::size_t idx = active_indices[(active_cursor_ + k) % count];
      if (evaluate_npc_decision(npcs_[idx], metrics)) {
        ++metrics.intent_changes;
      }
      ++metrics.evaluated_this_tick;
    }
    active_cursor_ = (active_cursor_ + limit) % count;
  }

  if (!bg_indices.empty() && config_.max_background_evals_per_tick > 0) {
    const std::size_t count = bg_indices.size();
    std::uint32_t evaluated_bg = 0;
    std::size_t scanned = 0;
    for (; scanned < count && evaluated_bg < config_.max_background_evals_per_tick;
         ++scanned) {
      const std::size_t idx = bg_indices[(bg_cursor_ + scanned) % count];
      auto& npc = npcs_[idx];
      if (npc.last_eval_time_sec < 0.0 ||
          (clock_.total_seconds - npc.last_eval_time_sec) >=
              config_.background_eval_interval_sec) {
        if (evaluate_npc_decision(npc, metrics)) {
          ++metrics.intent_changes;
        }
        ++evaluated_bg;
        ++metrics.evaluated_this_tick;
      }
    }
    bg_cursor_ = (bg_cursor_ + scanned) % count;
  }

  if (!dormant_indices.empty() && config_.max_dormant_evals_per_tick > 0) {
    const std::size_t count = dormant_indices.size();
    std::uint32_t evaluated_dormant = 0;
    std::size_t scanned = 0;
    for (; scanned < count && evaluated_dormant < config_.max_dormant_evals_per_tick;
         ++scanned) {
      const std::size_t idx = dormant_indices[(dormant_cursor_ + scanned) % count];
      auto& npc = npcs_[idx];
      if (npc.last_eval_time_sec < 0.0 ||
          (clock_.total_seconds - npc.last_eval_time_sec) >=
              config_.dormant_eval_interval_sec) {
        if (evaluate_npc_decision(npc, metrics)) {
          ++metrics.intent_changes;
        }
        ++evaluated_dormant;
        ++metrics.evaluated_this_tick;
      }
    }
    dormant_cursor_ = (dormant_cursor_ + scanned) % count;
  }

  // Update physical staff & customer counts at each venue and accrue revenue
  // only when open, staffed, and customers are physically present.
  for (auto& v : venues_) {
    v.active_staff = 0;
    v.active_customers = 0;
  }
  for (const auto& npc : npcs_) {
    if (npc.reserved_venue == kInvalidVenueId) continue;
    Venue* v = find_venue_mut(npc.reserved_venue);
    if (v == nullptr) continue;
    const double dist = WorldPosition::distance(
        npc.position, v->anchor_position, config_.chunk_size_m);
    if (dist <= config_.venue_arrival_radius_m) {
      if (npc.current_nav_intent.intent == NpcIntent::Work) {
        ++v->active_staff;
      } else if (npc.current_nav_intent.intent == NpcIntent::Shop ||
                 npc.current_nav_intent.intent == NpcIntent::Socialize) {
        ++v->active_customers;
      }
    }
  }
  const std::uint16_t cur_min = clock_.current_minute_int();
  for (auto& v : venues_) {
    if (v.category == VenueCategory::Commercial ||
        v.category == VenueCategory::Hospitality) {
      const bool staffed =
          (v.staff_required == 0) || (v.active_staff >= v.staff_required);
      if (v.is_open(cur_min) && staffed && v.active_customers > 0) {
        v.revenue_units += v.active_customers;
      }
    }
  }

  return metrics;
}

}  // namespace saige::world_sim
