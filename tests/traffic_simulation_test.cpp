#include "saige/traffic_simulation.hpp"

#include <cstdlib>
#include <iostream>

#define SAIGE_CHECK(cond)                                                     \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::cerr << "SAIGE_CHECK FAILED at " << __FILE__ << ":" << __LINE__    \
                << " (" << #cond << ")\n";                                    \
      std::exit(1);                                                           \
    }                                                                         \
  } while (false)

using namespace saige::traffic;

namespace {

void test_signal_cycle_and_emergency_preemption() {
  TrafficSimulation sim;
  LaneSegment ns_lane{1, {0.0, -100.0}, {0.0, 0.0}, 3.5, 14.0, 1, kInvalidLaneId, kInvalidLaneId, {}};
  LaneSegment ew_lane{2, {-100.0, 0.0}, {0.0, 0.0}, 3.5, 14.0, 1, kInvalidLaneId, kInvalidLaneId, {}};
  SAIGE_CHECK(sim.add_lane(ns_lane));
  SAIGE_CHECK(sim.add_lane(ew_lane));

  IntersectionController inter{};
  inter.id = 1;
  inter.phases.push_back({{1}, 10.0, 2.0, 1.0});
  inter.phases.push_back({{2}, 10.0, 2.0, 1.0});
  SAIGE_CHECK(sim.add_intersection(inter));

  SAIGE_CHECK(sim.find_intersection(1)->signal_for_lane(1) == SignalColor::Green);
  SAIGE_CHECK(sim.find_intersection(1)->signal_for_lane(2) == SignalColor::Red);

  TrafficVehicle amb{};
  amb.id = 100;
  amb.role = VehicleRole::Emergency;
  amb.current_lane = 2;
  amb.s_m = 40.0;
  SAIGE_CHECK(sim.add_vehicle(amb));

  for (int i = 0; i < 40; ++i) {
    sim.step(0.1);
  }
  SAIGE_CHECK(sim.find_intersection(1)->signal_for_lane(2) == SignalColor::Green);
  SAIGE_CHECK(sim.find_intersection(1)->signal_for_lane(1) == SignalColor::Red);
  SAIGE_CHECK(sim.find_vehicle(100)->control_output.siren_active);
}

void test_idm_following_crosswalk_and_lane_change() {
  TrafficSimulation sim;
  LaneSegment lane_r{1, {0.0, 0.0}, {200.0, 0.0}, 3.5, 14.0, kInvalidIntersectionId, 2, kInvalidLaneId, {}};
  LaneSegment lane_l{2, {0.0, 3.5}, {200.0, 3.5}, 3.5, 14.0, kInvalidIntersectionId, kInvalidLaneId, 1, {}};
  SAIGE_CHECK(sim.add_lane(lane_r));
  SAIGE_CHECK(sim.add_lane(lane_l));
  SAIGE_CHECK(sim.add_crosswalk({10, 2, 120.0, 124.0, true}));

  TrafficVehicle stalled{};
  stalled.id = 1;
  stalled.current_lane = 1;
  stalled.s_m = 30.0;
  stalled.speed_mps = 0.0;
  stalled.desired_speed_mps = 0.0;
  SAIGE_CHECK(sim.add_vehicle(stalled));

  TrafficVehicle ego{};
  ego.id = 2;
  ego.current_lane = 1;
  ego.s_m = 22.0;
  ego.speed_mps = 2.0;
  SAIGE_CHECK(sim.add_vehicle(ego));

  std::uint32_t total_lane_changes = 0;
  for (int i = 0; i < 100; ++i) {
    const auto m = sim.step(0.1);
    total_lane_changes += m.lane_changes;
  }
  SAIGE_CHECK(total_lane_changes >= 1);
  SAIGE_CHECK(sim.find_vehicle(2)->current_lane == 2);
  SAIGE_CHECK(sim.find_vehicle(2)->s_m <= 120.0);
}

// Regression test for Build 001 Defect 3: speed limit recovery on faster successor lane
void test_speed_limit_recovery_on_faster_lane() {
  TrafficSimulation sim;
  LaneSegment slow_lane{1, {0.0, 0.0}, {40.0, 0.0}, 3.5, 5.0, kInvalidIntersectionId, kInvalidLaneId, kInvalidLaneId, {2}};
  LaneSegment fast_lane{2, {40.0, 0.0}, {240.0, 0.0}, 3.5, 20.0, kInvalidIntersectionId, kInvalidLaneId, kInvalidLaneId, {}};
  SAIGE_CHECK(sim.add_lane(slow_lane));
  SAIGE_CHECK(sim.add_lane(fast_lane));

  TrafficVehicle v{};
  v.id = 1;
  v.current_lane = 1;
  v.s_m = 5.0;
  v.speed_mps = 5.0;
  v.desired_speed_mps = 18.0;
  SAIGE_CHECK(sim.add_vehicle(v));

  // Transition through slow_lane onto fast_lane
  for (int i = 0; i < 120; ++i) {
    sim.step(0.1);
  }
  SAIGE_CHECK(sim.find_vehicle(1)->current_lane == 2);
  SAIGE_CHECK(sim.find_vehicle(1)->speed_mps > 12.0);
  SAIGE_CHECK(sim.find_vehicle(1)->desired_speed_mps == 18.0);
}

// Regression test for Build 001 Defect 4: braking when exactly at occupied crosswalk start
void test_occupied_crosswalk_exact_boundary_braking() {
  TrafficSimulation sim;
  LaneSegment lane{1, {0.0, 0.0}, {100.0, 0.0}, 3.5, 14.0, kInvalidIntersectionId, kInvalidLaneId, kInvalidLaneId, {}};
  SAIGE_CHECK(sim.add_lane(lane));
  SAIGE_CHECK(sim.add_crosswalk({1, 1, 50.0, 54.0, true}));

  TrafficVehicle v{};
  v.id = 1;
  v.current_lane = 1;
  v.s_m = 50.0;  // Exactly at cw.s_start_m
  v.speed_mps = 6.0;
  SAIGE_CHECK(sim.add_vehicle(v));

  sim.step(0.1);
  SAIGE_CHECK(sim.find_vehicle(1)->control_output.brake > 0.5);
  SAIGE_CHECK(sim.find_vehicle(1)->control_output.brake_reason == BrakeReason::OccupiedCrosswalk);
  SAIGE_CHECK(sim.find_vehicle(1)->control_output.throttle == 0.0);
  SAIGE_CHECK(sim.find_vehicle(1)->speed_mps < 6.0);
}

// Regression test for Build 001 Defect 5: terminal lane stops at endpoint
void test_terminal_lane_stops_at_endpoint() {
  TrafficSimulation sim;
  LaneSegment dead_end{1, {0.0, 0.0}, {50.0, 0.0}, 3.5, 14.0, kInvalidIntersectionId, kInvalidLaneId, kInvalidLaneId, {}};
  SAIGE_CHECK(sim.add_lane(dead_end));

  TrafficVehicle v{};
  v.id = 1;
  v.current_lane = 1;
  v.s_m = 10.0;
  v.speed_mps = 10.0;
  SAIGE_CHECK(sim.add_vehicle(v));

  for (int i = 0; i < 100; ++i) {
    sim.step(0.1);
  }
  SAIGE_CHECK(sim.find_vehicle(1)->s_m <= 50.0);
  SAIGE_CHECK(sim.find_vehicle(1)->speed_mps < 0.1);
}

}  // namespace

void test_external_physics_feedback_ownership() {
  TrafficSimulation sim;
  SAIGE_CHECK(sim.add_lane({1, {0.0, 0.0}, {100.0, 0.0}, 3.5, 14.0, kInvalidIntersectionId, kInvalidLaneId, kInvalidLaneId, {}}));
  TrafficVehicle v{};
  v.id = 7;
  v.current_lane = 1;
  v.s_m = 10.0;
  v.speed_mps = 0.0;
  v.use_external_physics = true;
  SAIGE_CHECK(sim.add_vehicle(v));

  sim.step(0.1);
  // Planning emits throttle > 0, but does NOT mutate authoritative s_m or speed_mps
  SAIGE_CHECK(sim.find_vehicle(7)->control_output.throttle > 0.5);
  SAIGE_CHECK(sim.find_vehicle(7)->s_m == 10.0);
  SAIGE_CHECK(sim.find_vehicle(7)->speed_mps == 0.0);

  SAIGE_CHECK(sim.sync_external_vehicle_state(7, 1, 11.2, 3.4));
  SAIGE_CHECK(sim.find_vehicle(7)->s_m == 11.2);
  SAIGE_CHECK(sim.find_vehicle(7)->speed_mps == 3.4);
}

void test_external_physics_owns_lane_change_and_routing() {
  TrafficSimulation sim;
  SAIGE_CHECK(sim.add_lane({1, {0.0, 0.0}, {50.0, 0.0}, 3.5, 14.0, kInvalidIntersectionId, 2, kInvalidLaneId, {3}}));
  SAIGE_CHECK(sim.add_lane({2, {0.0, 3.5}, {50.0, 3.5}, 3.5, 14.0, kInvalidIntersectionId, kInvalidLaneId, 1, {}}));
  SAIGE_CHECK(sim.add_lane({3, {50.0, 0.0}, {120.0, 0.0}, 3.5, 14.0, kInvalidIntersectionId, kInvalidLaneId, kInvalidLaneId, {4}}));
  SAIGE_CHECK(sim.add_lane({4, {120.0, 0.0}, {200.0, 0.0}, 3.5, 14.0, kInvalidIntersectionId, kInvalidLaneId, kInvalidLaneId, {}}));

  const auto route = sim.find_lane_route(1, 4);
  SAIGE_CHECK(route.size() == 3);
  SAIGE_CHECK(route[0] == 1 && route[1] == 3 && route[2] == 4);

  TrafficVehicle lead{}; lead.id = 10; lead.current_lane = 1; lead.s_m = 20.0; lead.speed_mps = 0.0; lead.desired_speed_mps = 0.0;
  TrafficVehicle ego{}; ego.id = 11; ego.current_lane = 1; ego.s_m = 13.0; ego.speed_mps = 2.0;
  ego.use_external_physics = true;
  SAIGE_CHECK(sim.add_vehicle(lead));
  SAIGE_CHECK(sim.add_vehicle(ego));

  sim.step(0.1);
  // Planner requests lane change to lane 2, but does NOT mutate current_lane directly
  SAIGE_CHECK(sim.find_vehicle(11)->control_output.requested_lane_change == LaneChangeCommand::Left);
  SAIGE_CHECK(sim.find_vehicle(11)->control_output.target_lane_id == 2);
  SAIGE_CHECK(sim.find_vehicle(11)->current_lane == 1);

  SAIGE_CHECK(sim.sync_external_vehicle_state(11, 2, 13.5, 2.5));
  SAIGE_CHECK(sim.find_vehicle(11)->current_lane == 2);
}

int main() {
  test_signal_cycle_and_emergency_preemption();
  test_idm_following_crosswalk_and_lane_change();
  test_speed_limit_recovery_on_faster_lane();
  test_occupied_crosswalk_exact_boundary_braking();
  test_terminal_lane_stops_at_endpoint();
  test_external_physics_feedback_ownership();
  test_external_physics_owns_lane_change_and_routing();
  std::cout << "traffic_simulation_test: ALL 7 SUITES PASSED\n";
  return 0;
}
