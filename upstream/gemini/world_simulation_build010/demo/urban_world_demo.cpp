#include "saige/crowd_avoidance.hpp"
#include "saige/multi_floor_nav.hpp"
#include "saige/npc_simulation.hpp"
#include "saige/saige_world_adapter.hpp"
#include "saige/traffic_simulation.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

struct VehiclePhysicsProxy {
  double mass_kg = 1500.0;
  double max_drive_force_n = 4200.0;
  double max_brake_force_n = 9000.0;
  double drag_coeff = 0.35;
  saige::traffic::LaneId lane_id = 1;
  double s_m = 2.0;
  double speed_mps = 6.0;

  void integrate(const saige::traffic::VehicleControlInput& ctrl, double dt_sec) noexcept {
    if (ctrl.requested_lane_change != saige::traffic::LaneChangeCommand::None &&
        ctrl.target_lane_id != saige::traffic::kInvalidLaneId) {
      lane_id = ctrl.target_lane_id;
    }
    const double drive = ctrl.throttle * max_drive_force_n;
    const double brake = (ctrl.brake + (ctrl.handbrake ? 1.0 : 0.0)) * max_brake_force_n;
    const double drag = drag_coeff * speed_mps * speed_mps;
    const double net_force = drive - (speed_mps > 0.01 ? (brake + drag) : 0.0);
    speed_mps = std::max(0.0, speed_mps + (net_force / mass_kg) * dt_sec);
    s_m = std::min(120.0, s_m + speed_mps * dt_sec);
  }
};

int main() {
  using namespace saige;

  world_sim::WorldSimulation world;
  world_sim::Venue cafe{};
  cafe.id = 1;
  cafe.category = world_sim::VenueCategory::Hospitality;
  cafe.anchor_position = world_sim::WorldPosition::from_world_double({30.0, 10.0, 0.0});
  cafe.open_minute = 420;
  cafe.close_minute = 1320;
  cafe.capacity = 24;
  world.register_venue(cafe);

  crowd::CrowdSolver crowd;
  crowd.set_wall_segments({
      {1, {28.0, 12.0}, {28.0, 25.0}},
      {2, {28.0, 8.0}, {28.0, -5.0}},
  });
  crowd.set_doorways({{10, {28.0, 10.0}, {1.0, 0.0}, 2.0, 5.0}});

  struct NpcSyncState {
    world_sim::EntityId id = 0;
    crowd::Vec2d last_dest{};
    bool has_dest = false;
    std::size_t route_cursor = 0;
  };
  std::vector<NpcSyncState> sync_states;

  for (world_sim::EntityId i = 1; i <= 20; ++i) {
    const double sx = (i == 1) ? 25.0 : (static_cast<double>(i % 5) * 2.5 + 8.0);
    const double sy = (i == 1) ? 10.0 : (static_cast<double>(i / 5) * 2.5 + 5.0);
    world_sim::NpcAgent npc{};
    npc.id = i;
    npc.position = world_sim::WorldPosition::from_world_double({sx, sy, 0.0});
    npc.drives.hunger = 0.85;
    if (i == 2) {
      world_sim::WorldSimulation::assign_occupation_schedule(
          npc, world_sim::OccupationKind::Paramedic, 1, 1);
    } else {
      npc.schedule.push_back({420, 1320, 1, world_sim::NpcIntent::Shop});
    }
    world.register_npc(npc);

    crowd::CrowdAgent c{};
    c.id = i;
    c.position = {sx, sy};
    c.destination = (i == 1) ? crowd::Vec2d{26.0, 10.0} : crowd::Vec2d{30.0, 10.0};
    crowd.add_agent(c);
    sync_states.push_back({i, c.destination, true, 0});
  }

  multi_floor_nav::MultiFloorNavGraph nav(1.0, 0.35);
  for (int x = 25; x <= 35; ++x) {
    nav.set_slice({{x, 10, 0}, 0.0, true, x >= 29});
    if (x >= 30) nav.set_slice({{x, 10, 1}, 3.5, true, true});
  }
  nav.add_portal({1, {32, 10, 0}, {32, 10, 1}, multi_floor_nav::PortalKind::Stair, 4.0, 2.5, true});
  const auto npc1_route = nav.find_path({26, 10, 0}, {34, 10, 1});
  multi_floor_nav::PortalTraversalProgress npc1_portal{};

  traffic::TrafficSimulation traffic_sim;
  traffic_sim.add_lane({1, {0.0, 10.0}, {120.0, 10.0}, 3.5, 13.89, 1,
                        traffic::kInvalidLaneId, traffic::kInvalidLaneId, {}});
  traffic_sim.add_crosswalk({1, 1, 22.0, 26.0, false});
  traffic::IntersectionController inter{};
  inter.id = 1;
  inter.phases.push_back({{1}, 12.0, 3.0, 2.0});
  traffic_sim.add_intersection(inter);

  traffic::TrafficVehicle car{};
  car.id = 1;
  car.current_lane = 1;
  car.s_m = 2.0;
  car.speed_mps = 6.0;
  car.use_external_physics = true;
  traffic_sim.add_vehicle(car);
  VehiclePhysicsProxy car_physics{};

  constexpr double kDt = 1.0 / 60.0;
  double current_z_npc1 = 0.0;
  std::uint32_t crosswalk_brake_ticks = 0;
  std::uint32_t other_brake_ticks = 0;

  for (int tick = 0; tick < 900; ++tick) {
    if (tick == 300) {
      world.report_traffic_accident(99, world_sim::WorldPosition::from_world_double({18.0, 10.0, 0.0}), 10.0);
    }

    const auto wm = world.step(kDt, world_sim::WorldPosition::from_world_double({25.0, 10.0, 0.0}));

    // Advance smooth portal traversal for NPC #1 (drives character controller/animation)
    if (npc1_portal.is_active()) {
      npc1_portal.step(kDt);
      current_z_npc1 = npc1_portal.current_z_m();
    }

    for (auto& st : sync_states) {
      if (st.id == 1 && npc1_route.found && st.route_cursor < npc1_route.waypoints.size()) {
        if (npc1_portal.is_active()) continue;  // Wait for smooth stair traversal
        const auto wp = npc1_route.waypoints[st.route_cursor];
        const crowd::Vec2d wp_xy{static_cast<double>(wp.gx), static_cast<double>(wp.gy)};
        if (const auto* ca = crowd.find_agent(1)) {
          const double dx = ca->position.x - wp_xy.x;
          const double dy = ca->position.y - wp_xy.y;
          if ((dx * dx + dy * dy) < 0.85 && st.route_cursor + 1 < npc1_route.waypoints.size()) {
            const auto next_wp = npc1_route.waypoints[st.route_cursor + 1];
            if (const auto* portal = nav.find_portal_between(wp, next_wp)) {
              const auto* s_from = nav.find_slice(wp);
              const auto* s_to = nav.find_slice(next_wp);
              if (s_from && s_to) {
                npc1_portal.begin(portal->kind, wp, next_wp, s_from->z_m, s_to->z_m,
                                  portal->traversal_duration_sec);
              }
            }
            ++st.route_cursor;
            const crowd::Vec2d next_xy{static_cast<double>(next_wp.gx), static_cast<double>(next_wp.gy)};
            crowd.set_agent_destination(1, next_xy);
            st.last_dest = next_xy;
          }
        }
        continue;
      }

      const auto* npc = world.find_npc(st.id);
      if (npc != nullptr && npc->current_nav_intent.has_destination) {
        const auto d = npc->current_nav_intent.target_position.to_world_double();
        if (!st.has_dest || std::abs(d.x - st.last_dest.x) > 1e-3 ||
            std::abs(d.y - st.last_dest.y) > 1e-3) {
          crowd.set_agent_destination(st.id, {d.x, d.y});
          st.last_dest = {d.x, d.y};
          st.has_dest = true;
        }
      }
    }

    const auto cm = crowd.step(kDt);

    bool ped_in_crosswalk = false;
    for (const auto& ca : crowd.agents()) {
      if (auto* npc = world.find_npc(ca.id)) {
        const double z = (ca.id == 1) ? current_z_npc1 : 0.0;
        npc->position = world_sim::WorldPosition::from_world_double({ca.position.x, ca.position.y, z});
      }
      if (ca.position.x >= 22.0 && ca.position.x <= 26.0 &&
          ca.position.y >= 8.0 && ca.position.y <= 12.0) {
        ped_in_crosswalk = true;
      }
    }
    traffic_sim.set_crosswalk_occupied(1, ped_in_crosswalk);

    const auto tm = traffic_sim.step(kDt);
    if (const auto* tv = traffic_sim.find_vehicle(1)) {
      car_physics.integrate(tv->control_output, kDt);
      traffic_sim.sync_external_vehicle_state(1, car_physics.lane_id,
                                              car_physics.s_m, car_physics.speed_mps);
      if (tv->control_output.brake > 0.1) {
        if (tv->control_output.brake_reason == traffic::BrakeReason::OccupiedCrosswalk) {
          ++crosswalk_brake_ticks;
        } else {
          ++other_brake_ticks;
        }
      }
    }

    if (tick == 899) {
      std::cout << "[urban_world_demo tick=900] npcs=" << wm.total_npcs
                << " crowd_arrived=" << cm.arrived_agents
                << " crowd_max_overlap=" << cm.max_overlap_depth_m
                << "m npc1_z=" << current_z_npc1
                << "m car_s=" << car_physics.s_m
                << "m crosswalk_brake_ticks=" << crosswalk_brake_ticks
                << " other_brake_ticks=" << other_brake_ticks
                << " traffic_veh=" << tm.total_vehicles << "\n";
    }
  }
  return 0;
}
