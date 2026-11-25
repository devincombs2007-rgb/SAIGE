#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace saige::crowd {

using AgentId = std::uint64_t;
using ObstacleId = std::uint32_t;

struct Vec2d {
  double x = 0.0;
  double y = 0.0;
};

struct ObstacleSegment {
  ObstacleId id = 0;
  Vec2d a{};
  Vec2d b{};
};

struct DoorwayPortal {
  ObstacleId id = 0;
  Vec2d center{};
  Vec2d flow_axis{1.0, 0.0};  // Unit vector pointing through the doorway
  double half_width_m = 1.5;
  double trigger_radius_m = 4.5;
};

struct MovingObstacle {
  ObstacleId id = 0;
  Vec2d position{};
  Vec2d velocity{};
  double radius = 0.5;
};

struct CrowdAgent {
  AgentId id = 0;
  Vec2d position{};
  Vec2d velocity{};
  Vec2d destination{};
  bool has_destination = true;
  bool arrived = false;
  double body_radius = 0.30;    // Hard physical capsule radius (m)
  double personal_space = 0.12; // Preferred extra clearance (m)
  double max_speed = 1.6;       // m/s
  double max_accel = 6.0;       // m/s^2
  double arrival_radius = 0.50; // m
  double priority_weight = 1.0; // Higher weight yields less
  double stall_time_sec = 0.0;
  Vec2d prev_velocity{};
  std::uint32_t oscillation_count = 0;
};

struct CrowdSolverConfig {
  double neighbor_dist_m = 6.0;
  double time_horizon_agent_sec = 2.5;
  double time_horizon_obst_sec = 1.2;
  double spatial_cell_size_m = 4.0;
  std::uint32_t max_neighbors = 16;
  std::uint32_t separation_iterations = 2;
  double deadlock_speed_ratio = 0.10;
  double deadlock_stall_sec = 3.0;
};

struct CrowdStepMetrics {
  std::uint32_t total_agents = 0;
  std::uint32_t arrived_agents = 0;
  std::uint32_t overlap_pair_count = 0;
  double min_separation_m = 0.0;   // Edge-to-edge distance (< 0 = overlap)
  double max_overlap_depth_m = 0.0;
  std::uint32_t deadlock_count = 0;
  std::uint64_t oscillation_events = 0;
  std::size_t estimated_memory_bytes = 0;
};

class CrowdSolver {
 public:
  explicit CrowdSolver(CrowdSolverConfig config = {});

  bool add_agent(const CrowdAgent& agent);
  bool remove_agent(AgentId id);
  bool set_agent_destination(AgentId id, const Vec2d& destination);
  [[nodiscard]] CrowdAgent* find_agent(AgentId id) noexcept;
  [[nodiscard]] const CrowdAgent* find_agent(AgentId id) const noexcept;

  void set_wall_segments(std::vector<ObstacleSegment> segments);
  void set_moving_obstacles(std::vector<MovingObstacle> obstacles);
  void set_doorways(std::vector<DoorwayPortal> doorways);

  CrowdStepMetrics step(double dt_seconds);

  [[nodiscard]] const std::vector<CrowdAgent>& agents() const noexcept {
    return agents_;
  }

 private:
  struct OrcaLine {
    Vec2d point{};
    Vec2d direction{};
  };

  void rebuild_spatial_grid();
  void gather_neighbors(std::size_t agent_idx,
                        std::vector<std::size_t>& out_neighbors) const;
  void compute_agent_velocity(std::size_t agent_idx, double dt_seconds,
                              Vec2d& out_velocity) const;
  void resolve_physical_collisions();
  [[nodiscard]] CrowdStepMetrics collect_metrics() const;

  CrowdSolverConfig config_{};
  std::vector<CrowdAgent> agents_;
  std::vector<ObstacleSegment> walls_;
  std::vector<MovingObstacle> moving_obstacles_;
  std::vector<DoorwayPortal> doorways_;

  // Deterministic CSR-style spatial hash buckets
  std::uint32_t grid_bucket_count_ = 256;
  std::vector<std::int32_t> bucket_head_;
  std::vector<std::int32_t> agent_next_;
  std::vector<Vec2d> next_velocities_;
};

}  // namespace saige::crowd
