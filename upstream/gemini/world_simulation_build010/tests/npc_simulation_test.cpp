#include "saige/npc_simulation.hpp"
#include "saige/saige_world_adapter.hpp"

#include <cstdlib>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>

#define SAIGE_CHECK(cond)                                                     \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::cerr << "SAIGE_CHECK FAILED at " << __FILE__ << ":" << __LINE__    \
                << " (" << #cond << ")\n";                                    \
      std::exit(1);                                                           \
    }                                                                         \
  } while (false)

using namespace saige::world_sim;

namespace {

void test_starvation_and_fair_scheduling() {
  SimBudgetConfig cfg{};
  cfg.active_radius_m = 500.0;
  cfg.max_active_evals_per_tick = 16;

  WorldSimulation sim(cfg);
  for (std::uint32_t i = 1; i <= 120; ++i) {
    NpcAgent a{};
    a.id = i;
    a.position = WorldPosition::from_world_double({static_cast<double>(i), 0.0, 0.0});
    sim.register_npc(a);
  }

  for (int t = 0; t < 15; ++t) {
    sim.step(1.0 / 60.0, WorldPosition{});
  }
  for (const auto& npc : sim.npcs()) {
    SAIGE_CHECK(npc.evaluation_count == 2);
  }
}

void test_fair_scheduling_large_population() {
  SimBudgetConfig cfg{};
  cfg.active_radius_m = 100.0;
  cfg.background_radius_m = 400.0;
  cfg.max_active_evals_per_tick = 32;
  cfg.max_background_evals_per_tick = 32;
  cfg.background_eval_interval_sec = 0.0;

  WorldSimulation sim(cfg);
  for (std::uint32_t i = 1; i <= 1000; ++i) {
    NpcAgent a{};
    a.id = i;
    const double x = static_cast<double>((i % 350));
    a.position = WorldPosition::from_world_double({x, 0.0, 0.0});
    sim.register_npc(a);
  }

  for (int t = 0; t < 40; ++t) {
    sim.step(0.1, WorldPosition{});
  }

  std::uint64_t min_active_evals = std::numeric_limits<std::uint64_t>::max();
  std::uint64_t max_active_evals = 0;
  for (const auto& npc : sim.npcs()) {
    if (npc.lod == SimLodTier::Active) {
      min_active_evals = std::min(min_active_evals, npc.evaluation_count);
      max_active_evals = std::max(max_active_evals, npc.evaluation_count);
    }
  }
  SAIGE_CHECK(min_active_evals >= 1);
  SAIGE_CHECK((max_active_evals - min_active_evals) <= 1);
}

void test_overnight_and_day_rollover_transitions() {
  WorldSimulation sim;
  sim.clock().time_scale = 60.0;
  sim.clock().minute_of_day = 1435.0;
  sim.clock().day_index = 0;

  Venue night_shift{};
  night_shift.id = 5;
  night_shift.open_minute = 1320;
  night_shift.close_minute = 360;
  SAIGE_CHECK(sim.register_venue(night_shift));

  NpcAgent guard{};
  guard.id = 1;
  guard.schedule.push_back({1320, 360, 5, NpcIntent::Work});
  SAIGE_CHECK(sim.register_npc(guard));

  for (int i = 0; i < 10; ++i) {
    sim.step(1.0, WorldPosition{});
  }
  SAIGE_CHECK(sim.clock().day_index == 1);
  SAIGE_CHECK(sim.clock().current_minute_int() == 5);
  SAIGE_CHECK(sim.find_npc(1)->current_nav_intent.intent == NpcIntent::Work);
}

void test_venue_capacity_and_competing_npcs() {
  WorldSimulation sim;
  sim.clock().minute_of_day = 600.0;

  Venue cafe{};
  cafe.id = 1;
  cafe.capacity = 2;
  cafe.open_minute = 480;
  cafe.close_minute = 1200;
  cafe.anchor_position = WorldPosition::from_world_double({10.0, 10.0, 0.0});
  SAIGE_CHECK(sim.register_venue(cafe));

  Venue park{};
  park.id = 2;
  park.capacity = 10;
  park.open_minute = 0;
  park.close_minute = 0;
  park.anchor_position = WorldPosition::from_world_double({30.0, 10.0, 0.0});
  SAIGE_CHECK(sim.register_venue(park));

  for (EntityId id = 1; id <= 5; ++id) {
    NpcAgent a{};
    a.id = id;
    a.drives.hunger = 0.95;
    a.schedule.push_back({480, 1200, 1, NpcIntent::Shop});
    a.schedule.push_back({480, 1200, 2, NpcIntent::Socialize});
    SAIGE_CHECK(sim.register_npc(a));
  }

  const SimStepMetrics m = sim.step(1.0 / 60.0, WorldPosition{});
  SAIGE_CHECK(sim.find_venue(1)->current_occupants == 2);
  SAIGE_CHECK(sim.find_venue(2)->current_occupants == 3);
  SAIGE_CHECK(m.venue_capacity_denials >= 3);
  SAIGE_CHECK(sim.find_npc(1)->current_nav_intent.intent == NpcIntent::Shop);
  SAIGE_CHECK(sim.find_npc(2)->current_nav_intent.intent == NpcIntent::Shop);
  SAIGE_CHECK(sim.find_npc(3)->current_nav_intent.intent == NpcIntent::Socialize);
}

void test_persistent_memory_across_world_streaming() {
  WorldSimulation sim;
  NpcAgent npc{};
  npc.id = 9;
  npc.position = WorldPosition{{3, -2}, {12.0f, 8.0f, 1.5f}};
  npc.persistent_memory.home_venue = 42;
  npc.persistent_memory.work_venue = 84;
  SAIGE_CHECK(sim.register_npc(npc));

  const WorldPosition focus = npc.position;
  sim.step(0.1, focus);
  SAIGE_CHECK(sim.find_npc(9)->lod == SimLodTier::Active);

  SensoryEvent alert{};
  alert.kind = SensoryKind::EmergencyAlert;
  alert.source_id = 777;
  alert.position = npc.position;
  alert.intensity = 0.9;
  SAIGE_CHECK(sim.emit_sensory_event(alert) == 1);
  SAIGE_CHECK(sim.find_npc(9)->short_term_memory.size() == 1);

  sim.set_chunk_loaded({3, -2}, false);
  sim.step(0.1, focus);
  SAIGE_CHECK(sim.find_npc(9)->lod == SimLodTier::Dormant);
  SAIGE_CHECK(sim.find_npc(9)->short_term_memory.empty());
  SAIGE_CHECK(sim.find_npc(9)->persistent_memory.home_venue == 42);
  SAIGE_CHECK(sim.find_npc(9)->persistent_memory.last_threat_source == 777);
  SAIGE_CHECK(sim.find_npc(9)->persistent_memory.disturbance_count == 1);

  sim.set_chunk_loaded({3, -2}, true);
  sim.step(0.1, focus);
  SAIGE_CHECK(sim.find_npc(9)->lod == SimLodTier::Active);
  SAIGE_CHECK(sim.find_npc(9)->persistent_memory.work_venue == 84);
}

void test_lod_transitions_and_hysteresis() {
  SimBudgetConfig cfg{};
  cfg.active_radius_m = 50.0;
  cfg.background_radius_m = 150.0;
  cfg.lod_hysteresis_m = 5.0;

  WorldSimulation sim(cfg);
  NpcAgent npc{};
  npc.id = 42;
  npc.position = WorldPosition::from_world_double({48.0, 0.0, 0.0});
  SAIGE_CHECK(sim.register_npc(npc));

  sim.step(0.1, WorldPosition{});
  SAIGE_CHECK(sim.find_npc(42)->lod == SimLodTier::Active);

  sim.find_npc(42)->position = WorldPosition::from_world_double({53.0, 0.0, 0.0});
  sim.step(0.1, WorldPosition{});
  SAIGE_CHECK(sim.find_npc(42)->lod == SimLodTier::Active);

  sim.find_npc(42)->position = WorldPosition::from_world_double({56.0, 0.0, 0.0});
  sim.step(0.1, WorldPosition{});
  SAIGE_CHECK(sim.find_npc(42)->lod == SimLodTier::Background);

  sim.find_npc(42)->position = WorldPosition::from_world_double({47.0, 0.0, 0.0});
  sim.step(0.1, WorldPosition{});
  SAIGE_CHECK(sim.find_npc(42)->lod == SimLodTier::Background);

  sim.find_npc(42)->position = WorldPosition::from_world_double({44.0, 0.0, 0.0});
  sim.step(0.1, WorldPosition{});
  SAIGE_CHECK(sim.find_npc(42)->lod == SimLodTier::Active);
}

void test_z_up_xy_chunk_coordinates() {
  // Verify Z is vertical elevation and X/Y are horizontal chunk coordinates
  const WorldPosition p = WorldPosition::from_world_double({130.0, -70.0, 12.5}, 64.0);
  SAIGE_CHECK(p.cell.x == 2);
  SAIGE_CHECK(p.cell.y == -2);
  SAIGE_CHECK(std::abs(p.local.x - 2.0f) < 1e-4f);
  SAIGE_CHECK(std::abs(p.local.y - 58.0f) < 1e-4f);
  SAIGE_CHECK(std::abs(p.local.z - 12.5f) < 1e-4f);

  const Vec3d w = p.to_world_double(64.0);
  SAIGE_CHECK(std::abs(w.x - 130.0) < 1e-4);
  SAIGE_CHECK(std::abs(w.y - (-70.0)) < 1e-4);
  SAIGE_CHECK(std::abs(w.z - 12.5) < 1e-4);
}

// Regression test for Build 001 Defect 1: INT32_MIN vs INT32_MAX overflow
void test_extreme_chunk_coord_no_overflow() {
  const WorldPosition min_pos{{std::numeric_limits<std::int32_t>::min(),
                               std::numeric_limits<std::int32_t>::min()},
                              {0.0f, 0.0f, 0.0f}};
  const WorldPosition max_pos{{std::numeric_limits<std::int32_t>::max(),
                               std::numeric_limits<std::int32_t>::max()},
                              {0.0f, 0.0f, 0.0f}};
  const double dist = WorldPosition::distance(min_pos, max_pos, 64.0);
  SAIGE_CHECK(std::isfinite(dist));
  SAIGE_CHECK(dist > 1e10);

  WorldSimulation sim;
  NpcAgent a{};
  a.id = 1;
  a.position = min_pos;
  SAIGE_CHECK(sim.register_npc(a));
  const SimStepMetrics m = sim.step(1.0 / 60.0, max_pos);
  SAIGE_CHECK(m.dormant_count == 1);
}

void test_deterministic_replay() {
  auto run_scenario = []() {
    SimBudgetConfig cfg{};
    cfg.max_active_evals_per_tick = 32;
    cfg.max_background_evals_per_tick = 16;
    WorldSimulation sim(cfg);

    Venue v{};
    v.id = 1;
    v.capacity = 40;
    v.anchor_position = WorldPosition::from_world_double({20.0, 20.0, 0.0});
    sim.register_venue(v);

    for (EntityId i = 1; i <= 150; ++i) {
      NpcAgent a{};
      a.id = i;
      a.position = WorldPosition::from_world_double(
          {static_cast<double>((i * 11) % 240), 0.0, 0.0});
      a.personality.bravery = static_cast<double>(i % 10) * 0.1;
      a.schedule.push_back({480, 1080, 1, NpcIntent::Work});
      sim.register_npc(a);
    }

    SimStepMetrics last{};
    for (int t = 0; t < 120; ++t) {
      if (t == 30) {
        SensoryEvent e{};
        e.kind = SensoryKind::PhysicalDisturbance;
        e.source_id = 9999;
        e.position = WorldPosition::from_world_double({15.0, 0.0, 0.0});
        e.intensity = 0.85;
        sim.emit_sensory_event(e);
      }
      last = sim.step(1.0 / 60.0, WorldPosition{});
    }
    return std::pair{last, sim.npcs()};
  };

  const auto [m1, npcs1] = run_scenario();
  const auto [m2, npcs2] = run_scenario();
  SAIGE_CHECK(m1.active_count == m2.active_count);
  SAIGE_CHECK(m1.background_count == m2.background_count);
  SAIGE_CHECK(m1.dormant_count == m2.dormant_count);
  SAIGE_CHECK(m1.evaluated_this_tick == m2.evaluated_this_tick);
  SAIGE_CHECK(m1.intent_changes == m2.intent_changes);
  SAIGE_CHECK(m1.venue_capacity_denials == m2.venue_capacity_denials);
  SAIGE_CHECK(npcs1.size() == npcs2.size());
  for (std::size_t i = 0; i < npcs1.size(); ++i) {
    SAIGE_CHECK(npcs1[i].id == npcs2[i].id);
    SAIGE_CHECK(npcs1[i].evaluation_count == npcs2[i].evaluation_count);
    SAIGE_CHECK(npcs1[i].current_nav_intent.intent == npcs2[i].current_nav_intent.intent);
    SAIGE_CHECK(npcs1[i].reserved_venue == npcs2[i].reserved_venue);
  }
}

void test_invalid_inputs_and_resource_limits() {
  SimBudgetConfig cfg{};
  cfg.max_active_evals_per_tick = 0;
  cfg.max_background_evals_per_tick = 0;
  cfg.max_dormant_evals_per_tick = 0;
  WorldSimulation sim(cfg);

  Venue bad_v{};
  bad_v.id = kInvalidVenueId;
  SAIGE_CHECK(!sim.register_venue(bad_v));
  NpcAgent bad_a{};
  bad_a.id = kInvalidEntityId;
  SAIGE_CHECK(!sim.register_npc(bad_a));

  NpcAgent bad_pos{};
  bad_pos.id = 1;
  bad_pos.position.local.x = std::numeric_limits<float>::quiet_NaN();
  SAIGE_CHECK(!sim.register_npc(bad_pos));

  NpcAgent zero_mem{};
  zero_mem.id = 2;
  zero_mem.max_short_term_memories = 0;
  SAIGE_CHECK(sim.register_npc(zero_mem));
  SAIGE_CHECK(!sim.register_npc(zero_mem));

  SensoryEvent e{};
  e.kind = SensoryKind::Auditory;
  e.intensity = 0.8;
  SAIGE_CHECK(sim.emit_sensory_event(e) == 1);
  SAIGE_CHECK(sim.find_npc(2)->short_term_memory.empty());

  const SimStepMetrics m_neg = sim.step(-0.1, WorldPosition{});
  SAIGE_CHECK(m_neg.evaluated_this_tick == 0);
  const SimStepMetrics m_zero_budget = sim.step(0.1, WorldPosition{});
  SAIGE_CHECK(m_zero_budget.evaluated_this_tick == 0);
  SAIGE_CHECK(sim.remove_npc(2));
  SAIGE_CHECK(!sim.remove_npc(2));
}

}  // namespace

void test_multiple_simultaneous_world_events_no_repeat_reaction() {
  WorldSimulation sim;
  // 25 simultaneous world events (> previous 16-entry bound)
  for (std::uint32_t i = 1; i <= 25; ++i) {
    WorldEventRecord ev{100 + i, SensoryKind::EmergencyAlert,
                        WorldPosition::from_world_double({10.0, 0.0, 0.0}),
                        30.0, 0.8, 20.0};
    SAIGE_CHECK(sim.register_world_event(ev));
  }

  NpcAgent npc{};
  npc.id = 1;
  npc.position = WorldPosition::from_world_double({11.0, 0.0, 0.0});
  SAIGE_CHECK(sim.register_npc(npc));

  // Step 10 ticks -> each of the 25 events reacted to exactly once (no eviction churn)
  for (int t = 0; t < 10; ++t) {
    sim.step(0.5, npc.position);
  }
  SAIGE_CHECK(sim.find_npc(1)->reacted_world_events.size() == 25);
  SAIGE_CHECK(sim.find_npc(1)->world_event_reaction_count == 25);
}

void test_native_saige_coordinate_roundtrip() {
  struct MockSaigeCell { std::int32_t x = 0; std::int32_t y = 0; };
  struct MockSaigeLocal { float x = 0.0f; float y = 0.0f; float z = 0.0f; };
  struct MockSaigeWorldPosition { MockSaigeCell cell{}; MockSaigeLocal local{}; };

  MockSaigeWorldPosition native{{120, -45}, {31.25f, 48.5f, 7.25f}};
  SAIGE_CHECK(validate_native_roundtrip(native, 1e-3, 64.0));
  // Cross-cell-size conversion (64m native -> 32m sim -> 64m native)
  const WorldPosition sim_p = from_native_world_position(native, 64.0, 32.0);
  const MockSaigeWorldPosition back = to_native_world_position<MockSaigeWorldPosition>(sim_p, 32.0, 64.0);
  SAIGE_CHECK(back.cell.x == 120);
  SAIGE_CHECK(back.cell.y == -45);
  SAIGE_CHECK(std::abs(back.local.x - 31.25f) < 1e-3f);
}

void test_occupations_witnesses_and_paramedic_response() {
  WorldSimulation sim;
  sim.clock().minute_of_day = 540.0;  // 09:00
  Venue shop{};
  shop.id = 1;
  shop.category = VenueCategory::Commercial;
  shop.anchor_position = WorldPosition::from_world_double({10.0, 0.0, 0.0});
  shop.open_minute = 480;
  shop.close_minute = 1080;
  shop.capacity = 10;
  shop.staff_required = 1;

  Venue home{};
  home.id = 2;
  home.category = VenueCategory::Residential;
  home.anchor_position = WorldPosition::from_world_double({80.0, 0.0, 0.0});
  home.open_minute = 0;
  home.close_minute = 0;
  home.capacity = 10;
  SAIGE_CHECK(sim.register_venue(shop));
  SAIGE_CHECK(sim.register_venue(home));

  NpcAgent clerk{};
  clerk.id = 1;
  clerk.personality.bravery = 0.1;
  clerk.position = WorldPosition::from_world_double({10.0, 0.0, 0.0});
  WorldSimulation::assign_occupation_schedule(clerk, OccupationKind::Shopkeeper, 1, 2);

  NpcAgent medic{};
  medic.id = 2;
  medic.position = WorldPosition::from_world_double({15.0, 0.0, 0.0});
  WorldSimulation::assign_occupation_schedule(medic, OccupationKind::Paramedic, 1, 2);

  // Customer starts 50m away (reserved, not physically present -> 0 revenue)
  NpcAgent customer{};
  customer.id = 3;
  customer.position = WorldPosition::from_world_double({60.0, 0.0, 0.0});
  customer.drives.hunger = 0.9;
  customer.schedule.push_back({480, 1080, 1, NpcIntent::Shop});

  SAIGE_CHECK(sim.register_npc(clerk));
  SAIGE_CHECK(sim.register_npc(medic));
  SAIGE_CHECK(sim.register_npc(customer));

  sim.step(1.0, clerk.position);
  SAIGE_CHECK(sim.find_venue(1)->active_staff >= 1);
  SAIGE_CHECK(sim.find_venue(1)->active_customers == 0);
  SAIGE_CHECK(sim.find_venue(1)->revenue_units == 0);

  // Customer arrives physically at the shop -> revenue accrues
  sim.find_npc(3)->position = WorldPosition::from_world_double({11.0, 0.0, 0.0});
  sim.step(1.0, clerk.position);
  SAIGE_CHECK(sim.find_venue(1)->active_customers == 1);
  SAIGE_CHECK(sim.find_venue(1)->revenue_units >= 1);

  SAIGE_CHECK(sim.report_traffic_accident(700, WorldPosition::from_world_double({12.0, 0.0, 0.0}), 20.0));
  sim.step(1.0, clerk.position);
  SAIGE_CHECK(sim.find_npc(1)->current_nav_intent.intent == NpcIntent::Flee);
  SAIGE_CHECK(sim.find_npc(2)->current_nav_intent.intent == NpcIntent::Assist);
}

int main() {
  test_starvation_and_fair_scheduling();
  test_fair_scheduling_large_population();
  test_overnight_and_day_rollover_transitions();
  test_venue_capacity_and_competing_npcs();
  test_persistent_memory_across_world_streaming();
  test_lod_transitions_and_hysteresis();
  test_z_up_xy_chunk_coordinates();
  test_extreme_chunk_coord_no_overflow();
  test_deterministic_replay();
  test_invalid_inputs_and_resource_limits();
  test_multiple_simultaneous_world_events_no_repeat_reaction();
  test_native_saige_coordinate_roundtrip();
  test_occupations_witnesses_and_paramedic_response();
  std::cout << "npc_simulation_test: ALL 13 SUITES PASSED\n";
  return 0;
}

