#include "saige/persistent_world.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace saige::persistent_world {

namespace {

[[nodiscard]] float clamp01f(float v) noexcept {
  if (!std::isfinite(v) || v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

[[nodiscard]] std::size_t hash_chunk(world_sim::ChunkCoord c,
                                     std::size_t mask) noexcept {
  const std::uint64_t ux =
      static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.x));
  const std::uint64_t uy =
      static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.y));
  const std::uint64_t h =
      (ux * 0x9E3779B97F4A7C15ULL) ^ (uy * 0xBF58476D1CE4E5B9ULL);
  return static_cast<std::size_t>(h) & mask;
}

[[nodiscard]] std::uint32_t crc32_compute(const std::uint8_t* data,
                                          std::size_t len) noexcept {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b) {
      const std::uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
      crc = (crc >> 1) ^ mask;
    }
  }
  return ~crc;
}

void write_u8(std::vector<std::uint8_t>& out, std::uint8_t v) { out.push_back(v); }
void write_u16_le(std::vector<std::uint8_t>& out, std::uint16_t v) {
  out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
}
void write_u32_le(std::vector<std::uint8_t>& out, std::uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
  }
}
void write_i32_le(std::vector<std::uint8_t>& out, std::int32_t v) {
  write_u32_le(out, static_cast<std::uint32_t>(v));
}
void write_u64_le(std::vector<std::uint8_t>& out, std::uint64_t v) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((v >> (i * 8)) & 0xFFu));
  }
}
void write_i64_le(std::vector<std::uint8_t>& out, std::int64_t v) {
  write_u64_le(out, static_cast<std::uint64_t>(v));
}
void write_f32_le(std::vector<std::uint8_t>& out, float v) {
  std::uint32_t u = 0;
  std::memcpy(&u, &v, sizeof(u));
  write_u32_le(out, u);
}
void write_f64_le(std::vector<std::uint8_t>& out, double v) {
  std::uint64_t u = 0;
  std::memcpy(&u, &v, sizeof(u));
  write_u64_le(out, u);
}

bool read_u8(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint8_t& v) {
  if (off + 1 > in.size()) return false;
  v = in[off++];
  return true;
}
bool read_u16_le(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint16_t& v) {
  if (off + 2 > in.size()) return false;
  v = static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[off]) |
                                 static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[off + 1]) << 8));
  off += 2;
  return true;
}
bool read_u32_le(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint32_t& v) {
  if (off + 4 > in.size()) return false;
  v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<std::uint32_t>(in[off + static_cast<std::size_t>(i)]) << (i * 8);
  }
  off += 4;
  return true;
}
bool read_i32_le(const std::vector<std::uint8_t>& in, std::size_t& off, std::int32_t& v) {
  std::uint32_t u = 0;
  if (!read_u32_le(in, off, u)) return false;
  v = static_cast<std::int32_t>(u);
  return true;
}
bool read_u64_le(const std::vector<std::uint8_t>& in, std::size_t& off, std::uint64_t& v) {
  if (off + 8 > in.size()) return false;
  v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<std::uint64_t>(in[off + static_cast<std::size_t>(i)]) << (i * 8);
  }
  off += 8;
  return true;
}
bool read_i64_le(const std::vector<std::uint8_t>& in, std::size_t& off, std::int64_t& v) {
  std::uint64_t u = 0;
  if (!read_u64_le(in, off, u)) return false;
  v = static_cast<std::int64_t>(u);
  return true;
}
bool read_f32_le(const std::vector<std::uint8_t>& in, std::size_t& off, float& v) {
  std::uint32_t u = 0;
  if (!read_u32_le(in, off, u)) return false;
  std::memcpy(&v, &u, sizeof(v));
  return true;
}
bool read_f64_le(const std::vector<std::uint8_t>& in, std::size_t& off, double& v) {
  std::uint64_t u = 0;
  if (!read_u64_le(in, off, u)) return false;
  std::memcpy(&v, &u, sizeof(v));
  return true;
}

void write_world_pos(std::vector<std::uint8_t>& out,
                     const world_sim::WorldPosition& p) {
  write_i32_le(out, p.cell.x);
  write_i32_le(out, p.cell.y);
  write_f32_le(out, p.local.x);
  write_f32_le(out, p.local.y);
  write_f32_le(out, p.local.z);
}

bool read_world_pos(const std::vector<std::uint8_t>& in, std::size_t& off,
                    world_sim::WorldPosition& p) {
  return read_i32_le(in, off, p.cell.x) && read_i32_le(in, off, p.cell.y) &&
         read_f32_le(in, off, p.local.x) && read_f32_le(in, off, p.local.y) &&
         read_f32_le(in, off, p.local.z);
}

[[nodiscard]] double interval_overlap(double a0, double a1, double b0,
                                      double b1) noexcept {
  return std::max(0.0, std::min(a1, b1) - std::max(a0, b0));
}

[[nodiscard]] double shift_overlap_in_day(world_sim::OccupationKind occ,
                                          double d0, double d1) noexcept {
  if (occ == world_sim::OccupationKind::NightGuard) {
    return interval_overlap(d0, d1, 0.0, 360.0) +
           interval_overlap(d0, d1, 1320.0, 1440.0);
  }
  return interval_overlap(d0, d1, 480.0, 1020.0);
}

// Exact continuous interval arithmetic over [t0_sec, t1_sec] (Defect 5 fix)
[[nodiscard]] double compute_scheduled_work_minutes(
    world_sim::OccupationKind occ, double t0_sec, double t1_sec) noexcept {
  if (t1_sec <= t0_sec) return 0.0;
  if (occ == world_sim::OccupationKind::Unemployed) return 0.0;
  const double m0 = t0_sec / 60.0 + 480.0;  // Simulation clock starts at 08:00
  const double m1 = t1_sec / 60.0 + 480.0;
  if (occ == world_sim::OccupationKind::Paramedic) return m1 - m0;

  const auto day0 = static_cast<std::int64_t>(std::floor(m0 / 1440.0));
  const auto day1 = static_cast<std::int64_t>(std::floor(m1 / 1440.0));
  if (day0 == day1) {
    return shift_overlap_in_day(occ, m0 - static_cast<double>(day0) * 1440.0,
                                m1 - static_cast<double>(day0) * 1440.0);
  }
  double total = shift_overlap_in_day(
      occ, m0 - static_cast<double>(day0) * 1440.0, 1440.0);
  const std::int64_t full_days = day1 - day0 - 1;
  if (full_days > 0) {
    const double per_day =
        (occ == world_sim::OccupationKind::NightGuard) ? 480.0 : 540.0;
    total += static_cast<double>(full_days) * per_day;
  }
  total += shift_overlap_in_day(
      occ, 0.0, m1 - static_cast<double>(day1) * 1440.0);
  return total;
}

// Exact Build 008 (WLD1 v1) record layouts for genuine v1->v2 migration
struct V1NpcRecord {
  world_sim::EntityId id;
  world_sim::WorldPosition position;
  world_sim::SimLodTier lod;
  world_sim::OccupationKind occupation;
  world_sim::VenueId home_venue;
  world_sim::VenueId work_venue;
  std::uint64_t owned_vehicle_id;
  std::int64_t balance_cents;
  float bravery;
  float sociability;
  float conscientiousness;
  float energy;
  float hunger;
  float stress;
  double last_simulated_sec;
};

struct V1RelRecord {
  world_sim::EntityId from_id;
  world_sim::EntityId to_id;
  RelationshipKind kind;
  float affinity;
  float familiarity;
  std::uint32_t interaction_count;
  double last_interaction_sec;
};

}  // namespace

PersistentWorldStore::PersistentWorldStore(double chunk_size_m,
                                           double active_radius_m,
                                           double background_radius_m)
    : chunk_size_m_(chunk_size_m > 1.0 ? chunk_size_m : 64.0),
      active_radius_m_(active_radius_m > 0.0 ? active_radius_m : 80.0),
      background_radius_m_(background_radius_m > active_radius_m
                               ? background_radius_m
                               : active_radius_m + 100.0) {
  rebuild_spatial_index();
}

void PersistentWorldStore::reserve_npcs(std::size_t count) {
  npcs_.reserve(count);
}

bool PersistentWorldStore::upsert_npc(const PersistentNpcRecord& rec) {
  if (rec.id == world_sim::kInvalidEntityId) return false;
  if (npcs_.empty() || npcs_.back().id < rec.id) {
    npcs_.push_back(rec);
    spatial_index_dirty_ = true;
    return true;
  }
  for (auto& existing : npcs_) {
    if (existing.id == rec.id) {
      existing = rec;
      spatial_index_dirty_ = true;
      return true;
    }
  }
  npcs_.push_back(rec);
  spatial_index_dirty_ = true;
  return true;
}

const PersistentNpcRecord* PersistentWorldStore::find_npc(
    world_sim::EntityId id) const noexcept {
  for (const auto& n : npcs_) {
    if (n.id == id) return &n;
  }
  return nullptr;
}

bool PersistentWorldStore::set_npc_position(
    world_sim::EntityId id, const world_sim::WorldPosition& pos) {
  for (auto& n : npcs_) {
    if (n.id == id) {
      if (!(n.position.cell == pos.cell)) {
        spatial_index_dirty_ = true;
      }
      n.position = pos;
      return true;
    }
  }
  return false;
}

bool PersistentWorldStore::set_npc_balance(world_sim::EntityId id,
                                           std::int64_t balance_cents) {
  for (auto& n : npcs_) {
    if (n.id == id) {
      n.balance_cents = balance_cents;
      return true;
    }
  }
  return false;
}

bool PersistentWorldStore::upsert_relationship(const RelationshipRecord& rel) {
  if (rel.from_id == world_sim::kInvalidEntityId ||
      rel.to_id == world_sim::kInvalidEntityId || rel.from_id == rel.to_id) {
    return false;
  }
  for (auto& r : relationships_) {
    if (r.from_id == rel.from_id && r.to_id == rel.to_id) {
      r = rel;
      return true;
    }
  }
  relationships_.push_back(rel);
  return true;
}

const RelationshipRecord* PersistentWorldStore::find_relationship(
    world_sim::EntityId from_id, world_sim::EntityId to_id) const noexcept {
  for (const auto& r : relationships_) {
    if (r.from_id == from_id && r.to_id == to_id) return &r;
  }
  return nullptr;
}

bool PersistentWorldStore::upsert_business(const BusinessRecord& biz) {
  if (biz.venue_id == world_sim::kInvalidVenueId) return false;
  for (auto& b : businesses_) {
    if (b.venue_id == biz.venue_id) {
      b = biz;
      return true;
    }
  }
  businesses_.push_back(biz);
  return true;
}

const BusinessRecord* PersistentWorldStore::find_business(
    world_sim::VenueId id) const noexcept {
  for (const auto& b : businesses_) {
    if (b.venue_id == id) return &b;
  }
  return nullptr;
}

bool PersistentWorldStore::add_event(const PersistentEventRecord& ev) {
  if (ev.id == 0) return false;
  for (auto& existing : events_) {
    if (existing.id == ev.id) {
      existing = ev;
      return true;
    }
  }
  events_.push_back(ev);
  return true;
}

void PersistentWorldStore::ensure_spatial_index() const {
  if (spatial_index_dirty_ || chunk_buckets_.empty()) {
    rebuild_spatial_index();
  }
}

void PersistentWorldStore::rebuild_spatial_index() const {
  const auto t0 = std::chrono::steady_clock::now();

  // Defect 5 fix: size table to >= 2x npcs_.size() (minimum 256) and dynamically
  // grow if unique chunk count exceeds 50% load factor, preventing infinite probe loops.
  std::size_t cap = 256;
  const std::size_t min_cap = std::max<std::size_t>(256, npcs_.size() * 2 + 64);
  while (cap < min_cap && cap < (1u << 22)) {
    cap <<= 1;
  }

  chunk_buckets_.assign(cap, ChunkBucket{});
  bucket_occupied_.assign(cap, false);
  std::size_t occupied_count = 0;

  auto rehash = [&](std::size_t new_cap) {
    std::vector<ChunkBucket> old_buckets = std::move(chunk_buckets_);
    std::vector<bool> old_occ = std::move(bucket_occupied_);
    chunk_buckets_.assign(new_cap, ChunkBucket{});
    bucket_occupied_.assign(new_cap, false);
    const std::size_t mask = new_cap - 1;
    for (std::size_t i = 0; i < old_buckets.size(); ++i) {
      if (!old_occ[i]) continue;
      std::size_t slot = hash_chunk(old_buckets[i].coord, mask);
      while (bucket_occupied_[slot]) {
        slot = (slot + 1) & mask;
      }
      bucket_occupied_[slot] = true;
      chunk_buckets_[slot] = std::move(old_buckets[i]);
    }
  };

  for (std::size_t i = 0; i < npcs_.size(); ++i) {
    if ((occupied_count + 1) * 2 > chunk_buckets_.size()) {
      rehash(chunk_buckets_.size() * 2);
    }
    const std::size_t mask = chunk_buckets_.size() - 1;
    const world_sim::ChunkCoord c = npcs_[i].position.cell;
    std::size_t slot = hash_chunk(c, mask);
    std::size_t probes = 0;
    while (bucket_occupied_[slot] && !(chunk_buckets_[slot].coord == c) &&
           probes < chunk_buckets_.size()) {
      slot = (slot + 1) & mask;
      ++probes;
    }
    if (!bucket_occupied_[slot]) {
      bucket_occupied_[slot] = true;
      chunk_buckets_[slot].coord = c;
      ++occupied_count;
    }
    chunk_buckets_[slot].npc_indices.push_back(i);
  }

  spatial_index_dirty_ = false;
  const auto t1 = std::chrono::steady_clock::now();
  last_index_rebuild_ms_ =
      std::chrono::duration<double, std::milli>(t1 - t0).count();
}

std::size_t PersistentWorldStore::find_bucket_idx(
    world_sim::ChunkCoord c) const noexcept {
  if (chunk_buckets_.empty()) return 0;
  const std::size_t mask = chunk_buckets_.size() - 1;
  std::size_t slot = hash_chunk(c, mask);
  std::size_t probes = 0;
  while (bucket_occupied_[slot] && probes < chunk_buckets_.size()) {
    if (chunk_buckets_[slot].coord == c) return slot;
    slot = (slot + 1) & mask;
    ++probes;
  }
  return chunk_buckets_.size();
}

void PersistentWorldStore::query_chunk_npcs(
    world_sim::ChunkCoord chunk, std::vector<std::size_t>& out_indices) const {
  // Defect 2 fix: always ensure index is up-to-date before querying
  ensure_spatial_index();
  out_indices.clear();
  const std::size_t idx = find_bucket_idx(chunk);
  if (idx < chunk_buckets_.size()) {
    out_indices = chunk_buckets_[idx].npc_indices;
  }
}

std::uint32_t PersistentWorldStore::catch_up_chunk(world_sim::ChunkCoord chunk,
                                                   double current_time_sec) {
  ensure_spatial_index();
  std::vector<std::size_t> indices;
  query_chunk_npcs(chunk, indices);
  std::uint32_t caught_up = 0;
  for (std::size_t idx : indices) {
    auto& npc = npcs_[idx];
    const double elapsed = current_time_sec - npc.last_simulated_sec;
    if (elapsed > 0.0) {
      const float elapsed_min = static_cast<float>(elapsed / 60.0);
      npc.hunger = clamp01f(npc.hunger + 0.001f * elapsed_min);
      npc.energy = clamp01f(npc.energy - 0.0005f * elapsed_min);
      npc.stress = clamp01f(npc.stress - 0.01f * static_cast<float>(elapsed));
      // Schedule-aware dormant wage accrual
      const double worked_min = compute_scheduled_work_minutes(
          npc.occupation, npc.last_simulated_sec, current_time_sec);
      if (worked_min > 0.0) {
        npc.balance_cents +=
            static_cast<std::int64_t>(std::floor(worked_min * 25.0));
      }
      npc.last_simulated_sec = current_time_sec;
      ++caught_up;
    }
  }
  return caught_up;
}

StoreStepMetrics PersistentWorldStore::step_focused(
    double dt_seconds, const world_sim::WorldPosition& focus_pos,
    std::uint32_t max_active_evals, std::uint32_t max_bg_evals) {
  StoreStepMetrics m{};
  m.total_records = static_cast<std::uint32_t>(npcs_.size());
  if (!std::isfinite(dt_seconds) || dt_seconds <= 0.0) return m;

  current_time_sec_ += dt_seconds;
  ensure_spatial_index();
  m.index_rebuild_ms = last_index_rebuild_ms_;

  // Defect 3 fix: demote any previously Active/Background slots to Dormant
  // before re-evaluating the current focus area.
  for (std::size_t prev_idx : prev_focused_slots_) {
    if (prev_idx < npcs_.size()) {
      npcs_[prev_idx].lod = world_sim::SimLodTier::Dormant;
    }
  }
  prev_focused_slots_.clear();

  const std::int32_t span =
      static_cast<std::int32_t>(std::ceil(background_radius_m_ / chunk_size_m_)) + 1;

  std::vector<std::size_t> active_slots;
  std::vector<std::size_t> bg_slots;
  std::vector<std::size_t> chunk_indices;
  active_slots.reserve(256);
  bg_slots.reserve(512);

  for (std::int32_t dx = -span; dx <= span; ++dx) {
    for (std::int32_t dy = -span; dy <= span; ++dy) {
      const world_sim::ChunkCoord cc{focus_pos.cell.x + dx,
                                     focus_pos.cell.y + dy};
      query_chunk_npcs(cc, chunk_indices);
      for (std::size_t idx : chunk_indices) {
        auto& npc = npcs_[idx];
        const double dist = world_sim::WorldPosition::distance(
            npc.position, focus_pos, chunk_size_m_);
        if (dist <= active_radius_m_) {
          if (npc.lod == world_sim::SimLodTier::Dormant &&
              (current_time_sec_ - npc.last_simulated_sec) > 1.0) {
            m.caught_up_on_stream_in += catch_up_chunk(cc, current_time_sec_);
          }
          npc.lod = world_sim::SimLodTier::Active;
          active_slots.push_back(idx);
          prev_focused_slots_.push_back(idx);
        } else if (dist <= background_radius_m_) {
          npc.lod = world_sim::SimLodTier::Background;
          bg_slots.push_back(idx);
          prev_focused_slots_.push_back(idx);
        }
      }
    }
  }

  m.active_records = static_cast<std::uint32_t>(active_slots.size());
  m.background_records = static_cast<std::uint32_t>(bg_slots.size());
  m.dormant_records =
      m.total_records - m.active_records - m.background_records;

  // Defect 1 fix: fair rotating cursor for Active NPCs
  if (!active_slots.empty() && max_active_evals > 0) {
    const std::uint32_t active_limit =
        std::min<std::uint32_t>(max_active_evals, m.active_records);
    for (std::uint32_t k = 0; k < active_limit; ++k) {
      const std::size_t idx =
          active_slots[(active_cursor_ + k) % active_slots.size()];
      auto& npc = npcs_[idx];
      const double latency = current_time_sec_ - npc.last_simulated_sec;
      m.max_decision_latency_sec = std::max(m.max_decision_latency_sec, latency);
      npc.hunger = clamp01f(npc.hunger + 0.0001f * static_cast<float>(dt_seconds));
      npc.last_simulated_sec = current_time_sec_;
      ++npc.evaluation_count;
      ++m.evaluated_this_tick;
    }
    active_cursor_ = (active_cursor_ + active_limit) % active_slots.size();
  }

  if (!bg_slots.empty() && max_bg_evals > 0) {
    const std::uint32_t bg_limit =
        std::min<std::uint32_t>(max_bg_evals, m.background_records);
    for (std::uint32_t k = 0; k < bg_limit; ++k) {
      const std::size_t idx = bg_slots[(bg_cursor_ + k) % bg_slots.size()];
      npcs_[idx].last_simulated_sec = current_time_sec_;
      ++npcs_[idx].evaluation_count;
      ++m.evaluated_this_tick;
    }
    bg_cursor_ = (bg_cursor_ + bg_limit) % bg_slots.size();
  }

  std::size_t bucket_mem = chunk_buckets_.capacity() * sizeof(ChunkBucket);
  for (const auto& b : chunk_buckets_) {
    bucket_mem += b.npc_indices.capacity() * sizeof(std::size_t);
  }
  m.estimated_memory_bytes =
      npcs_.capacity() * sizeof(PersistentNpcRecord) +
      relationships_.capacity() * sizeof(RelationshipRecord) +
      businesses_.capacity() * sizeof(BusinessRecord) +
      events_.capacity() * sizeof(PersistentEventRecord) +
      bucket_mem + bucket_occupied_.capacity() +
      prev_focused_slots_.capacity() * sizeof(std::size_t);
  return m;
}

std::vector<std::uint8_t> PersistentWorldStore::serialize() const {
  std::vector<std::uint8_t> out;
  out.reserve(64 + npcs_.size() * 88 + relationships_.size() * 40);

  write_u32_le(out, kFormatMagic);
  write_u16_le(out, kFormatVersion);
  write_f64_le(out, current_time_sec_);
  write_u64_le(out, static_cast<std::uint64_t>(npcs_.size()));
  write_u64_le(out, static_cast<std::uint64_t>(relationships_.size()));
  write_u64_le(out, static_cast<std::uint64_t>(businesses_.size()));
  write_u64_le(out, static_cast<std::uint64_t>(events_.size()));

  for (const auto& r : npcs_) {
    write_u64_le(out, r.id);
    write_world_pos(out, r.position);
    write_u8(out, static_cast<std::uint8_t>(r.lod));
    write_u8(out, static_cast<std::uint8_t>(r.occupation));
    write_u32_le(out, r.home_venue);
    write_u32_le(out, r.work_venue);
    write_u64_le(out, r.owned_vehicle_id);
    write_i64_le(out, r.balance_cents);
    write_f32_le(out, r.bravery);
    write_f32_le(out, r.sociability);
    write_f32_le(out, r.conscientiousness);
    write_f32_le(out, r.energy);
    write_f32_le(out, r.hunger);
    write_f32_le(out, r.stress);
    write_f64_le(out, r.last_simulated_sec);
    write_u64_le(out, r.evaluation_count);
  }

  for (const auto& r : relationships_) {
    write_u64_le(out, r.from_id);
    write_u64_le(out, r.to_id);
    write_u8(out, static_cast<std::uint8_t>(r.kind));
    write_f32_le(out, r.affinity);
    write_f32_le(out, r.familiarity);
    write_f32_le(out, r.trust);
    write_u32_le(out, r.interaction_count);
    write_f64_le(out, r.last_interaction_sec);
  }

  for (const auto& b : businesses_) {
    write_u32_le(out, b.venue_id);
    write_u8(out, static_cast<std::uint8_t>(b.category));
    write_world_pos(out, b.anchor);
    write_u16_le(out, b.open_minute);
    write_u16_le(out, b.close_minute);
    write_u32_le(out, b.inventory_units);
    write_f64_le(out, b.unit_price);
    write_f64_le(out, b.demand_multiplier);
    write_u64_le(out, b.cumulative_revenue_cents);
  }

  for (const auto& e : events_) {
    write_u32_le(out, e.id);
    write_u8(out, static_cast<std::uint8_t>(e.kind));
    write_world_pos(out, e.position);
    write_f64_le(out, e.radius_m);
    write_f64_le(out, e.intensity);
    write_f64_le(out, e.expires_at_sec);
  }

  const std::uint32_t crc = crc32_compute(out.data(), out.size());
  write_u32_le(out, crc);
  return out;
}

bool PersistentWorldStore::deserialize(const std::vector<std::uint8_t>& blob) {
  // Minimum header (4+2+8+32 = 46 bytes)
  if (blob.size() < 46) return false;

  std::size_t peek_off = 0;
  std::uint32_t peek_magic = 0;
  std::uint16_t peek_ver = 0;
  if (!read_u32_le(blob, peek_off, peek_magic) ||
      !read_u16_le(blob, peek_off, peek_ver)) {
    return false;
  }

  // Genuine Build 008 WLD1 v1 migration path (no trailing CRC-32)
  if (peek_magic == kFormatMagicV1 && peek_ver == 1) {
    double saved_time = 0.0;
    std::uint64_t n_npcs = 0, n_rels = 0, n_biz = 0, n_ev = 0;
    if (!read_f64_le(blob, peek_off, saved_time) ||
        !read_u64_le(blob, peek_off, n_npcs) ||
        !read_u64_le(blob, peek_off, n_rels) ||
        !read_u64_le(blob, peek_off, n_biz) ||
        !read_u64_le(blob, peek_off, n_ev)) {
      return false;
    }
    if (n_npcs > kMaxSerializedRecords || n_rels > kMaxSerializedRecords ||
        n_biz > kMaxSerializedRecords || n_ev > kMaxSerializedRecords) {
      return false;
    }
    const std::size_t expected_size =
        peek_off +
        static_cast<std::size_t>(n_npcs) * sizeof(V1NpcRecord) +
        static_cast<std::size_t>(n_rels) * sizeof(V1RelRecord) +
        static_cast<std::size_t>(n_biz) * sizeof(BusinessRecord) +
        static_cast<std::size_t>(n_ev) * sizeof(PersistentEventRecord);
    if (expected_size != blob.size()) return false;

    std::vector<PersistentNpcRecord> new_npcs(static_cast<std::size_t>(n_npcs));
    for (std::size_t i = 0; i < new_npcs.size(); ++i) {
      V1NpcRecord v1{};
      std::memcpy(&v1, blob.data() + peek_off, sizeof(V1NpcRecord));
      peek_off += sizeof(V1NpcRecord);
      new_npcs[i] = {v1.id, v1.position, v1.lod, v1.occupation, v1.home_venue,
                     v1.work_venue, v1.owned_vehicle_id, v1.balance_cents,
                     v1.bravery, v1.sociability, v1.conscientiousness,
                     v1.energy, v1.hunger, v1.stress, v1.last_simulated_sec, 0};
    }
    std::vector<RelationshipRecord> new_rels(static_cast<std::size_t>(n_rels));
    for (std::size_t i = 0; i < new_rels.size(); ++i) {
      V1RelRecord v1{};
      std::memcpy(&v1, blob.data() + peek_off, sizeof(V1RelRecord));
      peek_off += sizeof(V1RelRecord);
      new_rels[i] = {v1.from_id, v1.to_id, v1.kind, v1.affinity,
                     v1.familiarity, 0.0f, v1.interaction_count,
                     v1.last_interaction_sec};
    }
    std::vector<BusinessRecord> new_biz(static_cast<std::size_t>(n_biz));
    for (std::size_t i = 0; i < new_biz.size(); ++i) {
      std::memcpy(&new_biz[i], blob.data() + peek_off, sizeof(BusinessRecord));
      peek_off += sizeof(BusinessRecord);
    }
    std::vector<PersistentEventRecord> new_ev(static_cast<std::size_t>(n_ev));
    for (std::size_t i = 0; i < new_ev.size(); ++i) {
      std::memcpy(&new_ev[i], blob.data() + peek_off, sizeof(PersistentEventRecord));
      peek_off += sizeof(PersistentEventRecord);
    }

    current_time_sec_ = saved_time;
    npcs_ = std::move(new_npcs);
    relationships_ = std::move(new_rels);
    businesses_ = std::move(new_biz);
    events_ = std::move(new_ev);
    prev_focused_slots_.clear();
    active_cursor_ = 0;
    bg_cursor_ = 0;
    rebuild_spatial_index();
    return true;
  }

  // WLD2 v2 path: minimum 46 header + 4 CRC = 50 bytes
  if (blob.size() < 50) return false;
  const std::size_t payload_len = blob.size() - 4;
  std::uint32_t expected_crc = 0;
  std::size_t crc_off = payload_len;
  if (!read_u32_le(blob, crc_off, expected_crc)) return false;
  if (crc32_compute(blob.data(), payload_len) != expected_crc) return false;

  std::size_t off = 0;
  std::uint32_t magic = 0;
  std::uint16_t version = 0;
  double saved_time = 0.0;
  std::uint64_t n_npcs = 0, n_rels = 0, n_biz = 0, n_ev = 0;

  if (!read_u32_le(blob, off, magic) || magic != kFormatMagic) return false;
  if (!read_u16_le(blob, off, version) || (version != 1 && version != kFormatVersion)) {
    return false;
  }
  if (!read_f64_le(blob, off, saved_time)) return false;
  if (!read_u64_le(blob, off, n_npcs) || !read_u64_le(blob, off, n_rels) ||
      !read_u64_le(blob, off, n_biz) || !read_u64_le(blob, off, n_ev)) {
    return false;
  }

  // Defect 4 fix: bound record counts against kMaxSerializedRecords and remaining bytes
  if (n_npcs > kMaxSerializedRecords || n_rels > kMaxSerializedRecords ||
      n_biz > kMaxSerializedRecords || n_ev > kMaxSerializedRecords) {
    return false;
  }
  const std::size_t rem_bytes = payload_len - off;
  constexpr std::size_t kMinNpcWireBytes = 86;
  constexpr std::size_t kMinRelWireBytes = (1 ? 37 : 41);
  constexpr std::size_t kMinBizWireBytes = 57;
  constexpr std::size_t kMinEvWireBytes = 49;
  const std::uint64_t min_needed =
      n_npcs * kMinNpcWireBytes + n_rels * kMinRelWireBytes +
      n_biz * kMinBizWireBytes + n_ev * kMinEvWireBytes;
  if (min_needed > rem_bytes) return false;

  std::vector<PersistentNpcRecord> new_npcs(static_cast<std::size_t>(n_npcs));
  std::vector<RelationshipRecord> new_rels(static_cast<std::size_t>(n_rels));
  std::vector<BusinessRecord> new_biz(static_cast<std::size_t>(n_biz));
  std::vector<PersistentEventRecord> new_ev(static_cast<std::size_t>(n_ev));

  for (std::size_t i = 0; i < new_npcs.size(); ++i) {
    auto& r = new_npcs[i];
    std::uint8_t lod_u8 = 0, occ_u8 = 0;
    if (!read_u64_le(blob, off, r.id) || !read_world_pos(blob, off, r.position) ||
        !read_u8(blob, off, lod_u8) || !read_u8(blob, off, occ_u8) ||
        !read_u32_le(blob, off, r.home_venue) || !read_u32_le(blob, off, r.work_venue) ||
        !read_u64_le(blob, off, r.owned_vehicle_id) ||
        !read_i64_le(blob, off, r.balance_cents) ||
        !read_f32_le(blob, off, r.bravery) || !read_f32_le(blob, off, r.sociability) ||
        !read_f32_le(blob, off, r.conscientiousness) ||
        !read_f32_le(blob, off, r.energy) || !read_f32_le(blob, off, r.hunger) ||
        !read_f32_le(blob, off, r.stress) ||
        !read_f64_le(blob, off, r.last_simulated_sec)) {
      return false;
    }
    if (version >= 2) {
      if (!read_u64_le(blob, off, r.evaluation_count)) return false;
    }
    r.lod = static_cast<world_sim::SimLodTier>(lod_u8);
    r.occupation = static_cast<world_sim::OccupationKind>(occ_u8);
  }

  for (std::size_t i = 0; i < new_rels.size(); ++i) {
    auto& r = new_rels[i];
    std::uint8_t kind_u8 = 0;
    if (!read_u64_le(blob, off, r.from_id) || !read_u64_le(blob, off, r.to_id) ||
        !read_u8(blob, off, kind_u8) || !read_f32_le(blob, off, r.affinity) ||
        !read_f32_le(blob, off, r.familiarity)) {
      return false;
    }
    if (version >= 2) {
      if (!read_f32_le(blob, off, r.trust)) return false;
    }
    if (!read_u32_le(blob, off, r.interaction_count) ||
        !read_f64_le(blob, off, r.last_interaction_sec)) {
      return false;
    }
    r.kind = static_cast<RelationshipKind>(kind_u8);
  }

  for (std::size_t i = 0; i < new_biz.size(); ++i) {
    auto& b = new_biz[i];
    std::uint8_t cat_u8 = 0;
    if (!read_u32_le(blob, off, b.venue_id) || !read_u8(blob, off, cat_u8) ||
        !read_world_pos(blob, off, b.anchor) ||
        !read_u16_le(blob, off, b.open_minute) ||
        !read_u16_le(blob, off, b.close_minute) ||
        !read_u32_le(blob, off, b.inventory_units) ||
        !read_f64_le(blob, off, b.unit_price) ||
        !read_f64_le(blob, off, b.demand_multiplier) ||
        !read_u64_le(blob, off, b.cumulative_revenue_cents)) {
      return false;
    }
    b.category = static_cast<world_sim::VenueCategory>(cat_u8);
  }

  for (std::size_t i = 0; i < new_ev.size(); ++i) {
    auto& e = new_ev[i];
    std::uint8_t kind_u8 = 0;
    if (!read_u32_le(blob, off, e.id) || !read_u8(blob, off, kind_u8) ||
        !read_world_pos(blob, off, e.position) ||
        !read_f64_le(blob, off, e.radius_m) ||
        !read_f64_le(blob, off, e.intensity) ||
        !read_f64_le(blob, off, e.expires_at_sec)) {
      return false;
    }
    e.kind = static_cast<world_sim::SensoryKind>(kind_u8);
  }

  current_time_sec_ = saved_time;
  npcs_ = std::move(new_npcs);
  relationships_ = std::move(new_rels);
  businesses_ = std::move(new_biz);
  events_ = std::move(new_ev);
  prev_focused_slots_.clear();
  active_cursor_ = 0;
  bg_cursor_ = 0;
  rebuild_spatial_index();
  return true;
}

}  // namespace saige::persistent_world
