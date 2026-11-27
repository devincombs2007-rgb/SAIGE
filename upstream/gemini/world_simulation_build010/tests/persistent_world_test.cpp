#include "saige/persistent_world.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <cstring>
#include <limits>

#define SAIGE_CHECK(cond)                                                     \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::cerr << "SAIGE_CHECK FAILED at " << __FILE__ << ":" << __LINE__    \
                << " (" << #cond << ")\n";                                    \
      std::exit(1);                                                           \
    }                                                                         \
  } while (false)

using namespace saige::persistent_world;
using namespace saige::world_sim;

namespace {

void test_versioned_serialization_roundtrip() {
  PersistentWorldStore store(64.0, 80.0, 220.0);

  PersistentNpcRecord npc{};
  npc.id = 42;
  npc.position = WorldPosition{{12, -7}, {15.5f, 22.0f, 1.0f}};
  npc.occupation = OccupationKind::Shopkeeper;
  npc.home_venue = 10;
  npc.work_venue = 20;
  npc.owned_vehicle_id = 9001;
  npc.balance_cents = 125000;
  SAIGE_CHECK(store.upsert_npc(npc));

  SAIGE_CHECK(store.upsert_relationship({42, 43, RelationshipKind::Family, 0.9f, 0.95f, 0.88f, 12, 100.0}));
  SAIGE_CHECK(store.upsert_business({20, VenueCategory::Commercial, WorldPosition{}, 480, 1080, 75, 19.5, 1.2, 45000}));
  SAIGE_CHECK(store.add_event({5, SensoryKind::EmergencyAlert, npc.position, 40.0, 0.8, 300.0}));

  const auto blob = store.serialize();
  SAIGE_CHECK(!blob.empty());

  PersistentWorldStore restored;
  SAIGE_CHECK(restored.deserialize(blob));
  SAIGE_CHECK(restored.npc_count() == 1);
  SAIGE_CHECK(restored.relationship_count() == 1);
  SAIGE_CHECK(restored.business_count() == 1);
  SAIGE_CHECK(restored.event_count() == 1);

  const auto* r_npc = restored.find_npc(42);
  SAIGE_CHECK(r_npc != nullptr);
  SAIGE_CHECK(r_npc->balance_cents == 125000);
  SAIGE_CHECK(r_npc->owned_vehicle_id == 9001);
  SAIGE_CHECK(restored.find_relationship(42, 43)->trust > 0.8f);

  // Regression 4: corrupted record count / CRC must fail cleanly without std::bad_alloc
  auto bad_blob = blob;
  bad_blob[16] = 0xFF;
  bad_blob[17] = 0xFF;
  bad_blob[18] = 0xFF;
  bad_blob[19] = 0x7F;
  SAIGE_CHECK(!restored.deserialize(bad_blob));
}

void test_dormant_chunk_elapsed_catch_up() {
  PersistentWorldStore store(64.0, 80.0, 220.0);
  PersistentNpcRecord worker{};
  worker.id = 1;
  worker.position = WorldPosition{{50, 50}, {10.0f, 10.0f, 0.0f}};
  worker.lod = SimLodTier::Dormant;
  worker.occupation = OccupationKind::OfficeWorker;  // Day shift 08:00-17:00
  worker.balance_cents = 10000;
  worker.last_simulated_sec = 0.0;
  SAIGE_CHECK(store.upsert_npc(worker));

  PersistentNpcRecord unemployed{};
  unemployed.id = 2;
  unemployed.position = WorldPosition{{50, 50}, {12.0f, 10.0f, 0.0f}};
  unemployed.lod = SimLodTier::Dormant;
  unemployed.occupation = OccupationKind::Unemployed;
  unemployed.balance_cents = 10000;
  unemployed.last_simulated_sec = 0.0;
  SAIGE_CHECK(store.upsert_npc(unemployed));

  // Defect 2 regression: query_chunk_npcs immediately after upsert returns both
  std::vector<std::size_t> q;
  store.query_chunk_npcs({50, 50}, q);
  SAIGE_CHECK(q.size() == 2);

  const std::uint32_t caught = store.catch_up_chunk({50, 50}, 600.0);
  SAIGE_CHECK(caught == 2);
  SAIGE_CHECK(store.find_npc(1)->balance_cents > 10000);
  SAIGE_CHECK(store.find_npc(2)->balance_cents == 10000);  // Unemployed earns 0
}

// Regression 1 & 3: Active fairness (no starvation) & focus-move LOD demotion
void test_active_fairness_and_focus_demotion() {
  PersistentWorldStore store(64.0, 80.0, 220.0);
  for (EntityId i = 1; i <= 40; ++i) {
    PersistentNpcRecord rec{};
    rec.id = i;
    rec.position = WorldPosition{{0, 0}, {10.0f, 10.0f, 0.0f}};
    SAIGE_CHECK(store.upsert_npc(rec));
  }

  const WorldPosition focus_near{{0, 0}, {10.0f, 10.0f, 0.0f}};
  for (int t = 0; t < 10; ++t) {
    store.step_focused(0.1, focus_near, 8, 8);
  }
  for (const auto& npc : store.npcs()) {
    SAIGE_CHECK(npc.evaluation_count == 2);
    SAIGE_CHECK(npc.lod == SimLodTier::Active);
  }

  // Teleport focus far away -> previously Active NPCs must demote to Dormant
  const WorldPosition focus_far{{100, 100}, {0.0f, 0.0f, 0.0f}};
  const auto m = store.step_focused(0.1, focus_far, 8, 8);
  SAIGE_CHECK(m.active_records == 0);
  SAIGE_CHECK(m.dormant_records == 40);
  for (const auto& npc : store.npcs()) {
    SAIGE_CHECK(npc.lod == SimLodTier::Dormant);
  }
}

// Regression 5: 3,000 unique chunks must not hang or overflow spatial hash
void test_3000_unique_chunks_spatial_hash() {
  PersistentWorldStore store(64.0, 80.0, 220.0);
  store.reserve_npcs(3000);
  for (std::int32_t i = 0; i < 3000; ++i) {
    PersistentNpcRecord rec{};
    rec.id = static_cast<EntityId>(i + 1);
    rec.position = WorldPosition{{i * 3, i * 5}, {1.0f, 1.0f, 0.0f}};
    SAIGE_CHECK(store.upsert_npc(rec));
  }
  std::vector<std::size_t> out;
  store.query_chunk_npcs({2999 * 3, 2999 * 5}, out);
  SAIGE_CHECK(out.size() == 1);
}

void test_large_scale_population_benchmarks() {
  const std::uint32_t pops[] = {100, 1000, 10000, 50000, 100000};
  for (std::uint32_t pop : pops) {
    const auto t_init0 = std::chrono::steady_clock::now();
    PersistentWorldStore store(64.0, 80.0, 220.0);
    store.reserve_npcs(pop);
    for (std::uint32_t i = 1; i <= pop; ++i) {
      PersistentNpcRecord rec{};
      rec.id = i;
      const auto cx = static_cast<std::int32_t>((i % 25) - 12);
      const auto cy = static_cast<std::int32_t>(((i / 25) % 25) - 12);
      rec.position = WorldPosition{{cx, cy}, {32.0f, 32.0f, 0.0f}};
      rec.occupation = static_cast<OccupationKind>(i % 5);
      store.upsert_npc(rec);
    }
    const WorldPosition focus{{0, 0}, {32.0f, 32.0f, 0.0f}};
    const auto warmup = store.step_focused(1.0 / 60.0, focus, 128, 64);
    const auto t_init1 = std::chrono::steady_clock::now();
    const double init_ms =
        std::chrono::duration<double, std::milli>(t_init1 - t_init0).count();

    double max_ms = 0.0;
    double total_ms = 0.0;
    double max_lat = 0.0;
    StoreStepMetrics last{};
    for (int t = 0; t < 30; ++t) {
      const auto t0 = std::chrono::steady_clock::now();
      last = store.step_focused(1.0 / 60.0, focus, 128, 64);
      const auto t1 = std::chrono::steady_clock::now();
      const double step_ms =
          std::chrono::duration<double, std::milli>(t1 - t0).count();
      total_ms += step_ms;
      max_ms = std::max(max_ms, step_ms);
      max_lat = std::max(max_lat, last.max_decision_latency_sec);
    }

    SAIGE_CHECK(last.total_records == pop);
    SAIGE_CHECK(last.active_records + last.background_records +
                    last.dormant_records == pop);

    std::cout << "[persistent-scale n=" << pop << "] init_ms=" << init_ms
              << " index_ms=" << warmup.index_rebuild_ms
              << " avg_ms=" << (total_ms / 30.0)
              << " max_ms=" << max_ms
              << " max_lat_s=" << max_lat
              << " mem_mb=" << (static_cast<double>(last.estimated_memory_bytes) / (1024.0 * 1024.0))
              << "\n" << std::flush;
  }
}

}  // namespace

void test_v1_wld1_save_migration() {
  // Construct an actual Build 008 WLD1 v1 binary blob
  struct V1Npc {
    EntityId id; WorldPosition position; SimLodTier lod; OccupationKind occupation;
    VenueId home_venue; VenueId work_venue; std::uint64_t owned_vehicle_id;
    std::int64_t balance_cents; float bravery; float sociability; float conscientiousness;
    float energy; float hunger; float stress; double last_simulated_sec;
  };
  struct V1Rel {
    EntityId from_id; EntityId to_id; RelationshipKind kind;
    float affinity; float familiarity; std::uint32_t interaction_count; double last_interaction_sec;
  };

  std::vector<std::uint8_t> blob;
  auto push_pod = [&](const auto& v) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
    blob.insert(blob.end(), p, p + sizeof(v));
  };
  const std::uint32_t magic = kFormatMagicV1;
  const std::uint16_t ver = 1;
  const double t = 123.5;
  const std::uint64_t n1 = 1, n2 = 1, n3 = 0, n4 = 0;
  push_pod(magic); push_pod(ver); push_pod(t);
  push_pod(n1); push_pod(n2); push_pod(n3); push_pod(n4);

  V1Npc npc{77, WorldPosition{{2, 3}, {1.0f, 2.0f, 0.0f}}, SimLodTier::Dormant,
            OccupationKind::Shopkeeper, 1, 2, 55, 88000, 0.5f, 0.5f, 0.5f, 0.8f, 0.2f, 0.1f, 100.0};
  V1Rel rel{77, 78, RelationshipKind::Friend, 0.7f, 0.8f, 4, 90.0};
  push_pod(npc); push_pod(rel);

  PersistentWorldStore restored;
  SAIGE_CHECK(restored.deserialize(blob));
  SAIGE_CHECK(restored.npc_count() == 1);
  SAIGE_CHECK(restored.find_npc(77)->balance_cents == 88000);
  SAIGE_CHECK(restored.find_relationship(77, 78)->kind == RelationshipKind::Friend);
}

void test_set_npc_position_and_exact_shift_wages() {
  PersistentWorldStore store(64.0, 80.0, 220.0);
  PersistentNpcRecord worker{};
  worker.id = 1;
  worker.position = WorldPosition{{0, 0}, {10.0f, 10.0f, 0.0f}};
  worker.occupation = OccupationKind::OfficeWorker;  // 08:00-17:00 (480..1020)
  worker.balance_cents = 0;
  // 16:59 (539 min * 60 = 32340s after 08:00 start)
  worker.last_simulated_sec = 539.0 * 60.0;
  SAIGE_CHECK(store.upsert_npc(worker));

  // Defect 2: move via set_npc_position -> spatial index updates immediately
  SAIGE_CHECK(store.set_npc_position(1, WorldPosition{{10, 10}, {5.0f, 5.0f, 0.0f}}));
  std::vector<std::size_t> q0, q10;
  store.query_chunk_npcs({0, 0}, q0);
  store.query_chunk_npcs({10, 10}, q10);
  SAIGE_CHECK(q0.empty());
  SAIGE_CHECK(q10.size() == 1);

  // Defect 5: 1 min before closure (16:59) + 9 min after (17:09 = 549 min * 60) -> exact 25 cents!
  store.catch_up_chunk({10, 10}, 549.0 * 60.0);
  SAIGE_CHECK(store.find_npc(1)->balance_cents == 25);
}

int main() {
  test_versioned_serialization_roundtrip();
  test_dormant_chunk_elapsed_catch_up();
  test_active_fairness_and_focus_demotion();
  test_3000_unique_chunks_spatial_hash();
  test_v1_wld1_save_migration();
  test_set_npc_position_and_exact_shift_wages();
  test_large_scale_population_benchmarks();
  std::cout << "persistent_world_test: ALL 7 SUITES PASSED\n";
  return 0;
}
