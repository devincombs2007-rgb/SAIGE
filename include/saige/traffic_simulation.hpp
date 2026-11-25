#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace saige::traffic {

using LaneId = std::uint32_t;
using IntersectionId = std::uint32_t;
using VehicleId = std::uint64_t;

inline constexpr LaneId kInvalidLaneId = 0;
inline constexpr IntersectionId kInvalidIntersectionId = 0;
inline constexpr VehicleId kInvalidVehicleId = 0;

struct Vec2d {
  double x = 0.0;
  double y = 0.0;
};

enum class SignalColor : std::uint8_t {
  Red = 0,
  Green = 1,
  Yellow = 2,
};

enum class BrakeReason : std::uint8_t {
  None = 0,
  LeadVehicle = 1,
  RedSignal = 2,
  OccupiedCrosswalk = 3,
  TerminalLane = 4,
};

enum class LaneChangeCommand : std::uint8_t {
  None = 0,
  Left = 1,
  Right = 2,
};

enum class VehicleRole : std::uint8_t {
  Civilian = 0,
  Commercial = 1,
  Emergency = 2,
};

struct CrosswalkZone {
  std::uint32_t id = 0;
  LaneId lane_id = kInvalidLaneId;
  double s_start_m = 0.0;
  double s_end_m = 4.0;
  bool pedestrian_present = false;
};

struct LaneSegment {
  LaneId id = kInvalidLaneId;
  Vec2d start{};
  Vec2d end{};
  double width_m = 3.5;
  double speed_limit_mps = 13.89;  // ~50 km/h
  IntersectionId end_intersection = kInvalidIntersectionId;
  LaneId left_adjacent = kInvalidLaneId;
  LaneId right_adjacent = kInvalidLaneId;
  std::vector<LaneId> successor_lanes;

  [[nodiscard]] double length_m() const noexcept;
  [[nodiscard]] Vec2d position_at_s(double s_m) const noexcept;
  [[nodiscard]] Vec2d tangent() const noexcept;
};

struct SignalPhase {
  std::vector<LaneId> green_lanes;
  double green_duration_sec = 15.0;
  double yellow_duration_sec = 3.0;
  double all_red_duration_sec = 2.0;
};

struct IntersectionController {
  IntersectionId id = kInvalidIntersectionId;
  std::vector<SignalPhase> phases;
  std::size_t active_phase_idx = 0;
  SignalColor current_color = SignalColor::Green;
  double phase_timer_sec = 0.0;
  bool emergency_preempted = false;
  LaneId preempt_lane = kInvalidLaneId;

  void step(double dt_seconds) noexcept;
  void request_emergency_preemption(LaneId priority_lane) noexcept;
  void clear_emergency_preemption() noexcept;
  [[nodiscard]] SignalColor signal_for_lane(LaneId lane_id) const noexcept;
};

// Compatible input contract for SAIGE's existing vehicle physics (src/vehicle.cpp)
struct VehicleControlInput {
  double throttle = 0.0;      // [0.0, 1.0]
  double brake = 0.0;         // [0.0, 1.0]
  double steering_rad = 0.0;  // [-max_steer, +max_steer]
  bool handbrake = false;
  bool siren_active = false;
  BrakeReason brake_reason = BrakeReason::None;
  LaneChangeCommand requested_lane_change = LaneChangeCommand::None;
  LaneId target_lane_id = kInvalidLaneId;
};

struct TrafficVehicle {
  VehicleId id = kInvalidVehicleId;
  VehicleRole role = VehicleRole::Civilian;
  LaneId current_lane = kInvalidLaneId;
  double s_m = 0.0;          // Distance along current lane
  double speed_mps = 0.0;
  double length_m = 4.6;
  double desired_speed_mps = 13.89;
  double max_accel_mps2 = 2.5;
  double comfort_decel_mps2 = 3.0;
  double min_gap_m = 2.5;
  double time_headway_sec = 1.5;
  double stall_timer_sec = 0.0;
  bool accident_flagged = false;
  bool use_external_physics = false;
  std::vector<LaneId> route_lanes;
  std::size_t route_cursor = 0;
  VehicleControlInput control_output{};
};

struct TrafficStepMetrics {
  std::uint32_t total_vehicles = 0;
  std::uint32_t emergency_vehicles = 0;
  std::uint32_t lane_changes = 0;
  std::uint32_t accidents_detected = 0;
  std::uint32_t congested_vehicles = 0;
};

class TrafficSimulation {
 public:
  bool add_lane(const LaneSegment& lane);
  bool add_intersection(const IntersectionController& intersection);
  bool add_crosswalk(const CrosswalkZone& crosswalk);
  bool set_crosswalk_occupied(std::uint32_t crosswalk_id, bool occupied);

  bool add_vehicle(const TrafficVehicle& vehicle);
  bool remove_vehicle(VehicleId id);
  bool sync_external_vehicle_state(VehicleId id, LaneId lane_id, double s_m, double speed_mps);
  [[nodiscard]] const TrafficVehicle* find_vehicle(VehicleId id) const noexcept;
  [[nodiscard]] const LaneSegment* find_lane(LaneId id) const noexcept;
  [[nodiscard]] const IntersectionController* find_intersection(
      IntersectionId id) const noexcept;

  [[nodiscard]] std::vector<LaneId> find_lane_route(LaneId start_lane, LaneId goal_lane) const;
  TrafficStepMetrics step(double dt_seconds);

  [[nodiscard]] const std::vector<TrafficVehicle>& vehicles() const noexcept {
    return vehicles_;
  }

 private:
  [[nodiscard]] double compute_idm_accel(const TrafficVehicle& ego,
                                         double target_speed_mps,
                                         double gap_m,
                                         double lead_speed_mps) const noexcept;
  [[nodiscard]] bool find_lead_vehicle(const TrafficVehicle& ego,
                                       LaneId lane_id, double s_start,
                                       double& out_gap_m,
                                       double& out_lead_speed) const noexcept;
  [[nodiscard]] bool try_lane_change(TrafficVehicle& ego);

  std::vector<LaneSegment> lanes_;
  std::vector<IntersectionController> intersections_;
  std::vector<CrosswalkZone> crosswalks_;
  std::vector<TrafficVehicle> vehicles_;
};

}  // namespace saige::traffic
