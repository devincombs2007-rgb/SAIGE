#include "saige/traffic_simulation.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace saige::traffic {

namespace {

[[nodiscard]] bool is_finite_vec2(const Vec2d& v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y);
}

[[nodiscard]] double clamp(double v, double lo, double hi) noexcept {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

}  // namespace

double LaneSegment::length_m() const noexcept {
  const double dx = end.x - start.x;
  const double dy = end.y - start.y;
  return std::sqrt(dx * dx + dy * dy);
}

Vec2d LaneSegment::position_at_s(double s_m) const noexcept {
  const double len = length_m();
  if (len <= 1e-6) return start;
  const double t = clamp(s_m / len, 0.0, 1.0);
  return {start.x + t * (end.x - start.x), start.y + t * (end.y - start.y)};
}

Vec2d LaneSegment::tangent() const noexcept {
  const double len = length_m();
  if (len <= 1e-6) return {1.0, 0.0};
  return {(end.x - start.x) / len, (end.y - start.y) / len};
}

void IntersectionController::step(double dt_seconds) noexcept {
  if (phases.empty() || !std::isfinite(dt_seconds) || dt_seconds <= 0.0) {
    return;
  }
  const SignalPhase& phase = phases[active_phase_idx];
  phase_timer_sec += dt_seconds;

  if (emergency_preempted && preempt_lane != kInvalidLaneId) {
    const bool has_priority =
        std::find(phase.green_lanes.begin(), phase.green_lanes.end(),
                  preempt_lane) != phase.green_lanes.end();
    if (!has_priority && current_color == SignalColor::Green) {
      current_color = SignalColor::Yellow;
      phase_timer_sec = 0.0;
    }
  }

  switch (current_color) {
    case SignalColor::Green:
      if (!emergency_preempted && phase_timer_sec >= phase.green_duration_sec) {
        current_color = SignalColor::Yellow;
        phase_timer_sec = 0.0;
      }
      break;
    case SignalColor::Yellow:
      if (phase_timer_sec >= phase.yellow_duration_sec) {
        current_color = SignalColor::Red;
        phase_timer_sec = 0.0;
      }
      break;
    case SignalColor::Red:
      if (phase_timer_sec >= phase.all_red_duration_sec) {
        if (emergency_preempted && preempt_lane != kInvalidLaneId) {
          for (std::size_t i = 0; i < phases.size(); ++i) {
            const auto& g = phases[i].green_lanes;
            if (std::find(g.begin(), g.end(), preempt_lane) != g.end()) {
              active_phase_idx = i;
              break;
            }
          }
        } else {
          active_phase_idx = (active_phase_idx + 1) % phases.size();
        }
        current_color = SignalColor::Green;
        phase_timer_sec = 0.0;
      }
      break;
  }
}

void IntersectionController::request_emergency_preemption(
    LaneId priority_lane) noexcept {
  emergency_preempted = true;
  preempt_lane = priority_lane;
}

void IntersectionController::clear_emergency_preemption() noexcept {
  emergency_preempted = false;
  preempt_lane = kInvalidLaneId;
}

SignalColor IntersectionController::signal_for_lane(
    LaneId lane_id) const noexcept {
  if (phases.empty()) return SignalColor::Green;
  const auto& g = phases[active_phase_idx].green_lanes;
  if (std::find(g.begin(), g.end(), lane_id) != g.end()) {
    return current_color;
  }
  return SignalColor::Red;
}

bool TrafficSimulation::add_lane(const LaneSegment& lane) {
  if (lane.id == kInvalidLaneId || !is_finite_vec2(lane.start) ||
      !is_finite_vec2(lane.end) || lane.length_m() <= 1e-3) {
    return false;
  }
  if (find_lane(lane.id) != nullptr) return false;
  lanes_.push_back(lane);
  std::sort(lanes_.begin(), lanes_.end(),
            [](const LaneSegment& a, const LaneSegment& b) { return a.id < b.id; });
  return true;
}

bool TrafficSimulation::add_intersection(
    const IntersectionController& intersection) {
  if (intersection.id == kInvalidIntersectionId) return false;
  if (find_intersection(intersection.id) != nullptr) return false;
  intersections_.push_back(intersection);
  std::sort(intersections_.begin(), intersections_.end(),
            [](const IntersectionController& a, const IntersectionController& b) {
              return a.id < b.id;
            });
  return true;
}

bool TrafficSimulation::add_crosswalk(const CrosswalkZone& crosswalk) {
  if (crosswalk.id == 0 || crosswalk.lane_id == kInvalidLaneId) return false;
  for (const auto& c : crosswalks_) {
    if (c.id == crosswalk.id) return false;
  }
  crosswalks_.push_back(crosswalk);
  return true;
}

bool TrafficSimulation::set_crosswalk_occupied(std::uint32_t crosswalk_id,
                                               bool occupied) {
  for (auto& c : crosswalks_) {
    if (c.id == crosswalk_id) {
      c.pedestrian_present = occupied;
      return true;
    }
  }
  return false;
}

bool TrafficSimulation::add_vehicle(const TrafficVehicle& vehicle) {
  if (vehicle.id == kInvalidVehicleId ||
      find_lane(vehicle.current_lane) == nullptr ||
      find_vehicle(vehicle.id) != nullptr) {
    return false;
  }
  vehicles_.push_back(vehicle);
  std::sort(vehicles_.begin(), vehicles_.end(),
            [](const TrafficVehicle& a, const TrafficVehicle& b) {
              return a.id < b.id;
            });
  return true;
}

bool TrafficSimulation::sync_external_vehicle_state(VehicleId id, LaneId lane_id,
                                                      double s_m, double speed_mps) {
  if (!std::isfinite(s_m) || !std::isfinite(speed_mps) || speed_mps < 0.0 ||
      find_lane(lane_id) == nullptr) {
    return false;
  }
  for (auto& v : vehicles_) {
    if (v.id == id) {
      v.current_lane = lane_id;
      v.s_m = s_m;
      v.speed_mps = speed_mps;
      return true;
    }
  }
  return false;
}

bool TrafficSimulation::remove_vehicle(VehicleId id) {
  const auto it = std::find_if(vehicles_.begin(), vehicles_.end(),
                               [id](const TrafficVehicle& v) { return v.id == id; });
  if (it == vehicles_.end()) return false;
  vehicles_.erase(it);
  return true;
}

const TrafficVehicle* TrafficSimulation::find_vehicle(
    VehicleId id) const noexcept {
  for (const auto& v : vehicles_) {
    if (v.id == id) return &v;
  }
  return nullptr;
}

const LaneSegment* TrafficSimulation::find_lane(LaneId id) const noexcept {
  for (const auto& l : lanes_) {
    if (l.id == id) return &l;
  }
  return nullptr;
}

const IntersectionController* TrafficSimulation::find_intersection(
    IntersectionId id) const noexcept {
  for (const auto& i : intersections_) {
    if (i.id == id) return &i;
  }
  return nullptr;
}

double TrafficSimulation::compute_idm_accel(
    const TrafficVehicle& ego, double target_speed_mps, double gap_m,
    double lead_speed_mps) const noexcept {
  const double v = std::max(0.0, ego.speed_mps);
  const double v0 = std::max(0.1, target_speed_mps);
  const double free_term = 1.0 - std::pow(v / v0, 4.0);

  if (!std::isfinite(gap_m)) {
    return ego.max_accel_mps2 * free_term;
  }

  const double dv = v - lead_speed_mps;
  const double sqrt_ab =
      std::sqrt(std::max(0.01, ego.max_accel_mps2 * ego.comfort_decel_mps2));
  const double s_star =
      ego.min_gap_m +
      std::max(0.0, v * ego.time_headway_sec + (v * dv) / (2.0 * sqrt_ab));
  const double safe_gap = std::max(0.1, gap_m);
  return ego.max_accel_mps2 *
         (free_term - (s_star / safe_gap) * (s_star / safe_gap));
}

bool TrafficSimulation::find_lead_vehicle(const TrafficVehicle& ego,
                                          LaneId lane_id, double s_start,
                                          double& out_gap_m,
                                          double& out_lead_speed) const noexcept {
  bool found = false;
  double best_gap = std::numeric_limits<double>::infinity();
  double best_speed = 0.0;

  for (const auto& other : vehicles_) {
    if (other.id == ego.id || other.current_lane != lane_id) continue;
    if (other.s_m <= s_start) continue;
    const double gap = other.s_m - s_start - other.length_m;
    if (gap < best_gap) {
      best_gap = gap;
      best_speed = other.speed_mps;
      found = true;
    }
  }
  if (found) {
    out_gap_m = best_gap;
    out_lead_speed = best_speed;
  }
  return found;
}

std::vector<LaneId> TrafficSimulation::find_lane_route(
    LaneId start_lane, LaneId goal_lane) const {
  if (find_lane(start_lane) == nullptr || find_lane(goal_lane) == nullptr) {
    return {};
  }
  if (start_lane == goal_lane) return {start_lane};

  std::vector<LaneId> visited;
  std::vector<std::pair<LaneId, LaneId>> parent;
  std::queue<LaneId> q;
  visited.push_back(start_lane);
  q.push(start_lane);

  bool found = false;
  while (!q.empty() && !found) {
    const LaneId cur = q.front();
    q.pop();
    const LaneSegment* seg = find_lane(cur);
    if (seg == nullptr) continue;
    for (LaneId nxt : seg->successor_lanes) {
      if (find_lane(nxt) == nullptr) continue;
      if (std::find(visited.begin(), visited.end(), nxt) != visited.end()) continue;
      visited.push_back(nxt);
      parent.push_back({nxt, cur});
      if (nxt == goal_lane) {
        found = true;
        break;
      }
      q.push(nxt);
    }
  }
  if (!found) return {};
  std::vector<LaneId> route;
  LaneId cur = goal_lane;
  while (cur != start_lane) {
    route.push_back(cur);
    for (const auto& kv : parent) {
      if (kv.first == cur) {
        cur = kv.second;
        break;
      }
    }
  }
  route.push_back(start_lane);
  std::reverse(route.begin(), route.end());
  return route;
}

bool TrafficSimulation::try_lane_change(TrafficVehicle& ego) {
  const LaneSegment* cur = find_lane(ego.current_lane);
  if (cur == nullptr) return false;
  const LaneId candidates[2] = {cur->left_adjacent, cur->right_adjacent};
  for (LaneId target_id : candidates) {
    if (target_id == kInvalidLaneId) continue;
    const LaneSegment* target_lane = find_lane(target_id);
    if (target_lane == nullptr) continue;

    bool clear = true;
    for (const auto& other : vehicles_) {
      if (other.id == ego.id || other.current_lane != target_id) continue;
      if (std::abs(other.s_m - ego.s_m) < (ego.length_m + ego.min_gap_m + 3.0)) {
        clear = false;
        break;
      }
    }
    if (clear) {
      const bool is_left = (target_id == cur->left_adjacent);
      ego.control_output.requested_lane_change =
          is_left ? LaneChangeCommand::Left : LaneChangeCommand::Right;
      ego.control_output.target_lane_id = target_id;
      ego.control_output.steering_rad = is_left ? -0.15 : 0.15;
      if (!ego.use_external_physics) {
        ego.current_lane = target_id;
      }
      return true;
    }
  }
  return false;
}

TrafficStepMetrics TrafficSimulation::step(double dt_seconds) {
  TrafficStepMetrics metrics{};
  metrics.total_vehicles = static_cast<std::uint32_t>(vehicles_.size());
  if (!std::isfinite(dt_seconds) || dt_seconds <= 0.0) {
    return metrics;
  }

  for (auto& inter : intersections_) {
    inter.clear_emergency_preemption();
  }
  for (const auto& v : vehicles_) {
    if (v.role == VehicleRole::Emergency) {
      ++metrics.emergency_vehicles;
      const LaneSegment* lane = find_lane(v.current_lane);
      if (lane != nullptr && lane->end_intersection != kInvalidIntersectionId) {
        for (auto& inter : intersections_) {
          if (inter.id == lane->end_intersection) {
            inter.request_emergency_preemption(lane->id);
          }
        }
      }
    }
  }

  for (auto& inter : intersections_) {
    inter.step(dt_seconds);
  }

  for (auto& v : vehicles_) {
    const LaneSegment* lane = find_lane(v.current_lane);
    if (lane == nullptr) continue;

    // Defect 3 fix: compute effective target speed without mutating v.desired_speed_mps
    const double emergency_mult = (v.role == VehicleRole::Emergency) ? 1.3 : 1.0;
    double target_speed =
        std::min(v.desired_speed_mps, lane->speed_limit_mps * emergency_mult);

    v.control_output.requested_lane_change = LaneChangeCommand::None;
    v.control_output.target_lane_id = kInvalidLaneId;
    v.control_output.steering_rad = 0.0;

    double min_gap = std::numeric_limits<double>::infinity();
    double lead_speed = target_speed;
    BrakeReason reason = BrakeReason::None;

    double veh_gap = std::numeric_limits<double>::infinity();
    double veh_speed = 0.0;
    if (find_lead_vehicle(v, v.current_lane, v.s_m, veh_gap, veh_speed)) {
      if (veh_gap < min_gap) {
        min_gap = veh_gap;
        lead_speed = veh_speed;
        reason = BrakeReason::LeadVehicle;
      }
    }

    bool hard_brake_crosswalk = false;
    for (const auto& cw : crosswalks_) {
      if (cw.lane_id == v.current_lane && cw.pedestrian_present &&
          v.s_m <= cw.s_end_m) {
        const double cw_gap = std::max(0.0, cw.s_start_m - v.s_m);
        if (cw_gap <= 0.05) {
          hard_brake_crosswalk = true;
          min_gap = 0.0;
          lead_speed = 0.0;
          reason = BrakeReason::OccupiedCrosswalk;
        } else if (cw_gap < min_gap) {
          min_gap = cw_gap;
          lead_speed = 0.0;
          reason = BrakeReason::OccupiedCrosswalk;
        }
      }
    }

    const double lane_len = lane->length_m();
    const double dist_to_end = std::max(0.0, lane_len - v.s_m);
    bool is_terminal = lane->successor_lanes.empty();
    bool is_red_signal = false;
    if (!is_terminal && lane->end_intersection != kInvalidIntersectionId) {
      const IntersectionController* inter =
          find_intersection(lane->end_intersection);
      if (inter != nullptr &&
          inter->signal_for_lane(lane->id) != SignalColor::Green) {
        is_red_signal = true;
      }
    }
    if ((is_terminal || is_red_signal) && dist_to_end < min_gap) {
      min_gap = dist_to_end;
      lead_speed = 0.0;
      reason = is_terminal ? BrakeReason::TerminalLane : BrakeReason::RedSignal;
    }

    if (std::isfinite(veh_gap) && veh_gap < 10.0 && veh_speed < 2.0 &&
        v.speed_mps < 3.0) {
      if (try_lane_change(v)) {
        ++metrics.lane_changes;
        lane = find_lane(v.current_lane);
        target_speed = std::min(v.desired_speed_mps,
                                lane->speed_limit_mps * emergency_mult);
      }
    }

    double accel = hard_brake_crosswalk
                       ? (-v.comfort_decel_mps2 * 2.0)
                       : compute_idm_accel(v, target_speed, min_gap, lead_speed);

    // Authoritative physics ownership: skip internal integration when external physics drives state
    if (!v.use_external_physics) {
      v.speed_mps = std::max(0.0, v.speed_mps + accel * dt_seconds);
      v.s_m += v.speed_mps * dt_seconds;

      if (lane != nullptr && v.s_m >= lane->length_m()) {
        bool transitioned = false;
        if (!lane->successor_lanes.empty()) {
          LaneId next_id = lane->successor_lanes.front();
        if (v.route_cursor + 1 < v.route_lanes.size() &&
            v.route_lanes[v.route_cursor] == lane->id) {
          const LaneId planned = v.route_lanes[v.route_cursor + 1];
          if (std::find(lane->successor_lanes.begin(), lane->successor_lanes.end(),
                        planned) != lane->successor_lanes.end()) {
            next_id = planned;
            ++v.route_cursor;
          }
        }
          if (find_lane(next_id) != nullptr) {
            v.s_m -= lane->length_m();
            v.current_lane = next_id;
            transitioned = true;
          }
        }
        if (!transitioned) {
          v.s_m = lane->length_m();
          v.speed_mps = 0.0;
          accel = std::min(accel, 0.0);
        }
      }
    }

    v.control_output.throttle =
        (accel > 0.0) ? clamp(accel / v.max_accel_mps2, 0.0, 1.0) : 0.0;
    v.control_output.brake_reason = (accel < 0.0) ? reason : BrakeReason::None;
    v.control_output.brake =
        (accel < 0.0) ? clamp(-accel / (v.comfort_decel_mps2 * 2.0), 0.0, 1.0) : 0.0;
    v.control_output.handbrake = (v.speed_mps < 0.05 && min_gap < 3.0);
    v.control_output.siren_active = (v.role == VehicleRole::Emergency);

    if (v.speed_mps < 1.0) {
      ++metrics.congested_vehicles;
      if (min_gap < 0.2) {
        v.stall_timer_sec += dt_seconds;
        if (v.stall_timer_sec >= 5.0) {
          v.accident_flagged = true;
        }
      } else {
        v.stall_timer_sec = 0.0;
      }
    } else {
      v.stall_timer_sec = 0.0;
    }
    if (v.accident_flagged) {
      ++metrics.accidents_detected;
    }
  }

  return metrics;
}

}  // namespace saige::traffic
