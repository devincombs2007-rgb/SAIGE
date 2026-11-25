#include "saige/crowd_avoidance.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace saige::crowd {

namespace {

inline constexpr double kEps = 1e-6;

[[nodiscard]] double dot(const Vec2d& a, const Vec2d& b) noexcept {
  return a.x * b.x + a.y * b.y;
}

[[nodiscard]] double det(const Vec2d& a, const Vec2d& b) noexcept {
  return a.x * b.y - a.y * b.x;
}

[[nodiscard]] double abs_sq(const Vec2d& v) noexcept {
  return v.x * v.x + v.y * v.y;
}

[[nodiscard]] double length(const Vec2d& v) noexcept {
  return std::sqrt(abs_sq(v));
}

[[nodiscard]] Vec2d normalize(const Vec2d& v) noexcept {
  const double l = length(v);
  if (l <= kEps) return {0.0, 0.0};
  return {v.x / l, v.y / l};
}

[[nodiscard]] Vec2d operator+(const Vec2d& a, const Vec2d& b) noexcept {
  return {a.x + b.x, a.y + b.y};
}

[[nodiscard]] Vec2d operator-(const Vec2d& a, const Vec2d& b) noexcept {
  return {a.x - b.x, a.y - b.y};
}

[[nodiscard]] Vec2d operator*(double s, const Vec2d& v) noexcept {
  return {s * v.x, s * v.y};
}

[[nodiscard]] bool is_finite_vec2(const Vec2d& v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y);
}

[[nodiscard]] Vec2d closest_point_on_segment(const Vec2d& p, const Vec2d& a,
                                             const Vec2d& b) noexcept {
  const Vec2d ab = b - a;
  const double len_sq = abs_sq(ab);
  if (len_sq <= kEps) {
    return a;
  }
  double t = dot(p - a, ab) / len_sq;
  t = std::clamp(t, 0.0, 1.0);
  return a + t * ab;
}

[[nodiscard]] std::uint32_t hash_cell(std::int32_t cx, std::int32_t cy,
                                      std::uint32_t bucket_mask) noexcept {
  const std::uint32_t ux = static_cast<std::uint32_t>(cx) * 73856093u;
  const std::uint32_t uy = static_cast<std::uint32_t>(cy) * 19349663u;
  return (ux ^ uy) & bucket_mask;
}

struct Line {
  Vec2d point{};
  Vec2d direction{};
};

bool linear_program_1(const std::vector<Line>& lines, std::size_t line_no,
                      double radius, const Vec2d& opt_vel, bool direction_opt,
                      Vec2d& result) noexcept {
  const double dot_product = dot(lines[line_no].point, lines[line_no].direction);
  const double discriminant =
      dot_product * dot_product + radius * radius - abs_sq(lines[line_no].point);

  if (discriminant < 0.0) {
    return false;
  }

  const double sqrt_disc = std::sqrt(discriminant);
  double t_left = -dot_product - sqrt_disc;
  double t_right = -dot_product + sqrt_disc;

  for (std::size_t i = 0; i < line_no; ++i) {
    const double denominator = det(lines[line_no].direction, lines[i].direction);
    const double numerator =
        det(lines[i].direction, lines[line_no].point - lines[i].point);

    if (std::abs(denominator) <= kEps) {
      if (numerator < 0.0) {
        return false;
      }
      continue;
    }

    const double t = numerator / denominator;
    if (denominator >= 0.0) {
      t_right = std::min(t_right, t);
    } else {
      t_left = std::max(t_left, t);
    }
    if (t_left > t_right) {
      return false;
    }
  }

  if (direction_opt) {
    if (dot(opt_vel, lines[line_no].direction) > 0.0) {
      result = lines[line_no].point + t_right * lines[line_no].direction;
    } else {
      result = lines[line_no].point + t_left * lines[line_no].direction;
    }
  } else {
    const double t =
        dot(lines[line_no].direction, opt_vel - lines[line_no].point);
    if (t < t_left) {
      result = lines[line_no].point + t_left * lines[line_no].direction;
    } else if (t > t_right) {
      result = lines[line_no].point + t_right * lines[line_no].direction;
    } else {
      result = lines[line_no].point + t * lines[line_no].direction;
    }
  }
  return true;
}

std::size_t linear_program_2(const std::vector<Line>& lines, double radius,
                             const Vec2d& opt_vel, bool direction_opt,
                             Vec2d& result) noexcept {
  if (direction_opt) {
    result = radius * opt_vel;
  } else if (abs_sq(opt_vel) > radius * radius) {
    result = radius * normalize(opt_vel);
  } else {
    result = opt_vel;
  }

  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (det(lines[i].direction, lines[i].point - result) > 0.0) {
      const Vec2d temp_result = result;
      if (!linear_program_1(lines, i, radius, opt_vel, direction_opt, result)) {
        result = temp_result;
        return i;
      }
    }
  }
  return lines.size();
}

void linear_program_3(const std::vector<Line>& lines, std::size_t num_obst_lines,
                      std::size_t begin_line, double radius, Vec2d& result) {
  double distance = 0.0;
  std::vector<Line> proj_lines;
  proj_lines.reserve(lines.size());

  for (std::size_t i = begin_line; i < lines.size(); ++i) {
    if (det(lines[i].direction, lines[i].point - result) > distance) {
      proj_lines.assign(lines.begin(),
                        lines.begin() + static_cast<std::ptrdiff_t>(num_obst_lines));

      for (std::size_t j = num_obst_lines; j < i; ++j) {
        Line line{};
        const double determinant = det(lines[i].direction, lines[j].direction);
        if (std::abs(determinant) <= kEps) {
          if (dot(lines[i].direction, lines[j].direction) > 0.0) {
            continue;
          }
          line.point = 0.5 * (lines[i].point + lines[j].point);
        } else {
          const double s =
              det(lines[j].direction, lines[i].point - lines[j].point) /
              determinant;
          line.point = lines[i].point + s * lines[i].direction;
        }
        line.direction = normalize(lines[j].direction - lines[i].direction);
        proj_lines.push_back(line);
      }

      const Vec2d temp_result = result;
      const Vec2d opt_dir{-lines[i].direction.y, lines[i].direction.x};
      if (linear_program_2(proj_lines, radius, opt_dir, true, result) <
          proj_lines.size()) {
        result = temp_result;
      }
      distance = det(lines[i].direction, lines[i].point - result);
    }
  }
}

void add_velocity_obstacle_line(const Vec2d& rel_pos, const Vec2d& rel_vel,
                                double combined_radius, double time_horizon,
                                double dt_seconds, double responsibility,
                                const Vec2d& self_vel,
                                std::vector<Line>& out_lines) {
  const double dist_sq = abs_sq(rel_pos);
  const double combined_r_sq = combined_radius * combined_radius;

  Vec2d u{};
  Vec2d dir{};

  if (dist_sq > combined_r_sq) {
    const double inv_tau = 1.0 / std::max(0.05, time_horizon);
    Vec2d w = rel_vel - inv_tau * rel_pos;
    // Tiny deterministic right-hand bias for collinear head-on symmetry breaking
    if (std::abs(det(rel_pos, rel_vel)) < 1e-5 && dot(rel_pos, rel_vel) > 0.0) {
      const Vec2d perp = normalize({rel_pos.y, -rel_pos.x});
      w = w + 0.02 * perp;
    }

    const double w_len_sq = abs_sq(w);
    const double dot_product = dot(w, rel_pos);

    if (dot_product < 0.0 &&
        dot_product * dot_product > combined_r_sq * w_len_sq) {
      const double w_len = std::sqrt(w_len_sq);
      const Vec2d unit_w = (w_len > kEps) ? (1.0 / w_len) * w : Vec2d{1.0, 0.0};
      dir = {unit_w.y, -unit_w.x};
      u = (combined_radius * inv_tau - w_len) * unit_w;
    } else {
      const double leg = std::sqrt(std::max(0.0, dist_sq - combined_r_sq));
      if (det(rel_pos, w) > 0.0) {
        dir = {
            (rel_pos.x * leg - rel_pos.y * combined_radius) / dist_sq,
            (rel_pos.x * combined_radius + rel_pos.y * leg) / dist_sq,
        };
      } else {
        dir = {
            -(rel_pos.x * leg + rel_pos.y * combined_radius) / dist_sq,
            -(-rel_pos.x * combined_radius + rel_pos.y * leg) / dist_sq,
        };
      }
      const double dot_p2 = dot(rel_vel, dir);
      u = dot_p2 * dir - rel_vel;
    }
  } else {
    const double inv_step = 1.0 / std::max(1e-3, dt_seconds);
    const Vec2d w = rel_vel - inv_step * rel_pos;
    const double w_len = length(w);
    const Vec2d unit_w = (w_len > kEps) ? (1.0 / w_len) * w : Vec2d{1.0, 0.0};
    dir = {unit_w.y, -unit_w.x};
    u = (combined_radius * inv_step - w_len) * unit_w;
  }

  Line line{};
  line.direction = dir;
  line.point = self_vel + responsibility * u;
  out_lines.push_back(line);
}

}  // namespace

CrowdSolver::CrowdSolver(CrowdSolverConfig config) : config_(config) {
  if (!std::isfinite(config_.spatial_cell_size_m) ||
      config_.spatial_cell_size_m <= 0.1) {
    config_.spatial_cell_size_m = 4.0;
  }
}

bool CrowdSolver::add_agent(const CrowdAgent& agent) {
  if (agent.id == 0 || !is_finite_vec2(agent.position) ||
      !is_finite_vec2(agent.destination) || agent.body_radius <= 0.0 ||
      agent.max_speed <= 0.0) {
    return false;
  }
  for (const auto& existing : agents_) {
    if (existing.id == agent.id) {
      return false;
    }
  }
  agents_.push_back(agent);
  std::sort(agents_.begin(), agents_.end(),
            [](const CrowdAgent& a, const CrowdAgent& b) { return a.id < b.id; });
  return true;
}

bool CrowdSolver::remove_agent(AgentId id) {
  const auto it = std::find_if(agents_.begin(), agents_.end(),
                               [id](const CrowdAgent& a) { return a.id == id; });
  if (it == agents_.end()) {
    return false;
  }
  agents_.erase(it);
  return true;
}

bool CrowdSolver::set_agent_destination(AgentId id, const Vec2d& destination) {
  if (!is_finite_vec2(destination)) {
    return false;
  }
  CrowdAgent* agent = find_agent(id);
  if (agent == nullptr) {
    return false;
  }
  agent->destination = destination;
  agent->has_destination = true;
  agent->arrived = false;
  agent->stall_time_sec = 0.0;
  return true;
}

CrowdAgent* CrowdSolver::find_agent(AgentId id) noexcept {
  for (auto& a : agents_) {
    if (a.id == id) return &a;
  }
  return nullptr;
}

const CrowdAgent* CrowdSolver::find_agent(AgentId id) const noexcept {
  for (const auto& a : agents_) {
    if (a.id == id) return &a;
  }
  return nullptr;
}

void CrowdSolver::set_wall_segments(std::vector<ObstacleSegment> segments) {
  walls_ = std::move(segments);
}

void CrowdSolver::set_moving_obstacles(std::vector<MovingObstacle> obstacles) {
  moving_obstacles_ = std::move(obstacles);
}

void CrowdSolver::set_doorways(std::vector<DoorwayPortal> doorways) {
  doorways_ = std::move(doorways);
}

void CrowdSolver::rebuild_spatial_grid() {
  std::uint32_t target = 256;
  while (target < agents_.size() * 2 && target < (1u << 16)) {
    target <<= 1;
  }
  grid_bucket_count_ = target;
  bucket_head_.assign(grid_bucket_count_, -1);
  agent_next_.assign(agents_.size(), -1);
  const std::uint32_t mask = grid_bucket_count_ - 1;
  const double inv_cell = 1.0 / config_.spatial_cell_size_m;

  for (std::size_t i = 0; i < agents_.size(); ++i) {
    const auto cx = static_cast<std::int32_t>(std::floor(agents_[i].position.x * inv_cell));
    const auto cy = static_cast<std::int32_t>(std::floor(agents_[i].position.y * inv_cell));
    const std::uint32_t h = hash_cell(cx, cy, mask);
    agent_next_[i] = bucket_head_[h];
    bucket_head_[h] = static_cast<std::int32_t>(i);
  }
}

void CrowdSolver::gather_neighbors(std::size_t agent_idx,
                                   std::vector<std::size_t>& out_neighbors) const {
  out_neighbors.clear();
  if (bucket_head_.empty() || agent_next_.size() != agents_.size()) {
    return;
  }
  const auto& self = agents_[agent_idx];
  const double inv_cell = 1.0 / config_.spatial_cell_size_m;
  const std::int32_t span = static_cast<std::int32_t>(
      std::ceil(config_.neighbor_dist_m * inv_cell));
  const std::int32_t cx0 =
      static_cast<std::int32_t>(std::floor(self.position.x * inv_cell));
  const std::int32_t cy0 =
      static_cast<std::int32_t>(std::floor(self.position.y * inv_cell));
  const std::uint32_t mask = grid_bucket_count_ - 1;
  const double range_sq = config_.neighbor_dist_m * config_.neighbor_dist_m;

  for (std::int32_t dx = -span; dx <= span; ++dx) {
    for (std::int32_t dy = -span; dy <= span; ++dy) {
      const std::uint32_t h = hash_cell(cx0 + dx, cy0 + dy, mask);
      for (std::int32_t cur = bucket_head_[h]; cur >= 0;
           cur = agent_next_[static_cast<std::size_t>(cur)]) {
        const auto idx = static_cast<std::size_t>(cur);
        if (idx == agent_idx) continue;
        const double d_sq = abs_sq(agents_[idx].position - self.position);
        if (d_sq <= range_sq) {
          out_neighbors.push_back(idx);
        }
      }
    }
  }

  // Sort & deduplicate for 100% deterministic neighbor order regardless of hash collisions
  std::sort(out_neighbors.begin(), out_neighbors.end());
  out_neighbors.erase(std::unique(out_neighbors.begin(), out_neighbors.end()),
                      out_neighbors.end());

  if (out_neighbors.size() > config_.max_neighbors) {
    std::partial_sort(
        out_neighbors.begin(),
        out_neighbors.begin() + static_cast<std::ptrdiff_t>(config_.max_neighbors),
        out_neighbors.end(), [&](std::size_t a, std::size_t b) {
          const double da = abs_sq(agents_[a].position - self.position);
          const double db = abs_sq(agents_[b].position - self.position);
          if (std::abs(da - db) > 1e-9) return da < db;
          return a < b;
        });
    out_neighbors.resize(config_.max_neighbors);
  }
}

void CrowdSolver::compute_agent_velocity(std::size_t agent_idx,
                                         double dt_seconds,
                                         Vec2d& out_velocity) const {
  const auto& self = agents_[agent_idx];
  Vec2d pref_vel{0.0, 0.0};
  if (self.has_destination && !self.arrived) {
    const Vec2d to_goal = self.destination - self.position;
    const double dist = length(to_goal);
    if (dist > self.arrival_radius) {
      double speed = std::min(self.max_speed, dist / std::max(dt_seconds, 0.05));
      // Doorway queue coordination: throttle approach if opposing agents occupy throat
      for (const auto& dw : doorways_) {
        const double dw_dist = length(dw.center - self.position);
        if (dw_dist <= dw.trigger_radius_m) {
          const Vec2d axis = normalize(dw.flow_axis);
          const double self_proj = dot(self.position - dw.center, axis);
          const double goal_proj = dot(to_goal, axis);
          const double sign = (goal_proj >= 0.0) ? 1.0 : -1.0;
          if ((self_proj * sign) < -0.8 && (self_proj * sign) > -3.5) {
            std::uint32_t opposing_in_throat = 0;
            for (const auto& other : agents_) {
              if (other.id == self.id || other.arrived) continue;
              const double o_proj = dot(other.position - dw.center, axis);
              if (std::abs(o_proj) < 1.2 && dot(other.velocity, axis) * sign < -0.2) {
                ++opposing_in_throat;
              }
            }
            if (opposing_in_throat >= 2) {
              speed *= 0.45;
            }
          }
          break;
        }
      }
      pref_vel = (speed / dist) * to_goal;
    }
  }

  std::vector<Line> orca_lines;
  orca_lines.reserve(walls_.size() + moving_obstacles_.size() +
                     config_.max_neighbors);

  // 1. Static wall segments (hard constraints)
  const double obst_range =
      config_.time_horizon_obst_sec * self.max_speed + self.body_radius + 0.5;
  for (const auto& wall : walls_) {
    const Vec2d closest = closest_point_on_segment(self.position, wall.a, wall.b);
    const Vec2d rel_pos = closest - self.position;
    const double dist = length(rel_pos);
    if (dist > obst_range) {
      continue;
    }
    add_velocity_obstacle_line(rel_pos, self.velocity, self.body_radius + 0.04,
                               config_.time_horizon_obst_sec, dt_seconds, 1.0,
                               self.velocity, orca_lines);
  }

  // 2. Moving obstacles (unyielding: agent takes 1.0 responsibility)
  for (const auto& obs : moving_obstacles_) {
    const Vec2d rel_pos = obs.position - self.position;
    if (abs_sq(rel_pos) >
        (config_.neighbor_dist_m + obs.radius) *
            (config_.neighbor_dist_m + obs.radius)) {
      continue;
    }
    const Vec2d rel_vel = self.velocity - obs.velocity;
    add_velocity_obstacle_line(rel_pos, rel_vel,
                               self.body_radius + obs.radius + 0.05,
                               config_.time_horizon_agent_sec, dt_seconds, 1.0,
                               self.velocity, orca_lines);
  }

  const std::size_t num_obst_lines = orca_lines.size();

  // 3. Reciprocal pedestrian neighbors
  std::vector<std::size_t> neighbors;
  gather_neighbors(agent_idx, neighbors);
  for (std::size_t n_idx : neighbors) {
    const auto& other = agents_[n_idx];
    const Vec2d rel_pos = other.position - self.position;
    const Vec2d rel_vel = self.velocity - other.velocity;
    const double combined_r = self.body_radius + other.body_radius +
                              self.personal_space + other.personal_space;
    const double total_w = self.priority_weight + other.priority_weight;
    const double resp =
        (total_w > kEps) ? (other.priority_weight / total_w) : 0.5;

    add_velocity_obstacle_line(rel_pos, rel_vel, combined_r,
                               config_.time_horizon_agent_sec, dt_seconds, resp,
                               self.velocity, orca_lines);
  }

  Vec2d solved_vel = pref_vel;
  const std::size_t line_fail =
      linear_program_2(orca_lines, self.max_speed, pref_vel, false, solved_vel);
  if (line_fail < orca_lines.size()) {
    linear_program_3(orca_lines, num_obst_lines, line_fail, self.max_speed,
                     solved_vel);
  }

  // Acceleration clamp
  const Vec2d dv = solved_vel - self.velocity;
  const double max_dv = self.max_accel * dt_seconds;
  const double dv_len = length(dv);
  if (dv_len > max_dv && dv_len > kEps) {
    out_velocity = self.velocity + (max_dv / dv_len) * dv;
  } else {
    out_velocity = solved_vel;
  }
}

void CrowdSolver::resolve_physical_collisions() {
  std::vector<std::size_t> neighbors;
  for (std::uint32_t iter = 0; iter < config_.separation_iterations; ++iter) {
    rebuild_spatial_grid();
    for (std::size_t i = 0; i < agents_.size(); ++i) {
      gather_neighbors(i, neighbors);
      for (std::size_t j : neighbors) {
        if (j <= i) continue;
        Vec2d delta = agents_[j].position - agents_[i].position;
        double dist = length(delta);
        const double min_dist = agents_[i].body_radius + agents_[j].body_radius;
        if (dist < min_dist) {
          Vec2d dir = (dist > kEps) ? (1.0 / dist) * delta : Vec2d{1.0, 0.0};
          const double push = 0.5 * (min_dist - dist + 1e-4);
          agents_[i].position = agents_[i].position - push * dir;
          agents_[j].position = agents_[j].position + push * dir;
        }
      }
      // Push out of static walls
      for (const auto& wall : walls_) {
        const Vec2d closest =
            closest_point_on_segment(agents_[i].position, wall.a, wall.b);
        const Vec2d delta = agents_[i].position - closest;
        const double dist = length(delta);
        if (dist < agents_[i].body_radius) {
          const Vec2d dir =
              (dist > kEps) ? (1.0 / dist) * delta : Vec2d{0.0, 1.0};
          const double push = agents_[i].body_radius - dist + 1e-4;
          agents_[i].position = agents_[i].position + push * dir;
        }
      }
      // Push out of moving obstacles
      for (const auto& obs : moving_obstacles_) {
        const Vec2d delta = agents_[i].position - obs.position;
        const double dist = length(delta);
        const double min_dist = agents_[i].body_radius + obs.radius;
        if (dist < min_dist) {
          const Vec2d dir =
              (dist > kEps) ? (1.0 / dist) * delta : Vec2d{1.0, 0.0};
          const double push = min_dist - dist + 1e-4;
          agents_[i].position = agents_[i].position + push * dir;
        }
      }
    }
  }
}

CrowdStepMetrics CrowdSolver::collect_metrics() const {
  CrowdStepMetrics m{};
  m.total_agents = static_cast<std::uint32_t>(agents_.size());
  m.min_separation_m = std::numeric_limits<double>::infinity();

  std::vector<std::size_t> neighbors;
  for (std::size_t i = 0; i < agents_.size(); ++i) {
    const auto& a = agents_[i];
    if (a.arrived) ++m.arrived_agents;
    if (!a.arrived && a.stall_time_sec >= config_.deadlock_stall_sec) {
      ++m.deadlock_count;
    }
    m.oscillation_events += a.oscillation_count;

    gather_neighbors(i, neighbors);
    for (std::size_t j : neighbors) {
      if (j <= i) continue;
      const double center_dist = length(agents_[j].position - a.position);
      const double sep = center_dist - (a.body_radius + agents_[j].body_radius);
      m.min_separation_m = std::min(m.min_separation_m, sep);
      if (sep < 0.0) {
        ++m.overlap_pair_count;
        m.max_overlap_depth_m = std::max(m.max_overlap_depth_m, -sep);
      }
    }
  }
  if (!std::isfinite(m.min_separation_m)) {
    m.min_separation_m = 0.0;
  }
  m.estimated_memory_bytes =
      agents_.capacity() * sizeof(CrowdAgent) +
      walls_.capacity() * sizeof(ObstacleSegment) +
      moving_obstacles_.capacity() * sizeof(MovingObstacle) +
      bucket_head_.capacity() * sizeof(std::int32_t) +
      agent_next_.capacity() * sizeof(std::int32_t) +
      next_velocities_.capacity() * sizeof(Vec2d);
  return m;
}

CrowdStepMetrics CrowdSolver::step(double dt_seconds) {
  rebuild_spatial_grid();
  if (!std::isfinite(dt_seconds) || dt_seconds <= 0.0) {
    return collect_metrics();
  }
  next_velocities_.resize(agents_.size());

  for (std::size_t i = 0; i < agents_.size(); ++i) {
    compute_agent_velocity(i, dt_seconds, next_velocities_[i]);
  }

  for (std::size_t i = 0; i < agents_.size(); ++i) {
    auto& a = agents_[i];
    a.prev_velocity = a.velocity;
    a.velocity = next_velocities_[i];
    a.position = a.position + dt_seconds * a.velocity;

    if (a.has_destination && !a.arrived) {
      const double dist_to_goal = length(a.destination - a.position);
      if (dist_to_goal <= a.arrival_radius) {
        a.arrived = true;
        a.velocity = {0.0, 0.0};
        a.stall_time_sec = 0.0;
      } else {
        const double spd = length(a.velocity);
        if (spd < a.max_speed * config_.deadlock_speed_ratio) {
          a.stall_time_sec += dt_seconds;
        } else {
          a.stall_time_sec = 0.0;
        }
        if (dot(a.prev_velocity, a.velocity) < -0.25 * a.max_speed * a.max_speed) {
          ++a.oscillation_count;
        }
      }
    }
  }

  resolve_physical_collisions();
  rebuild_spatial_grid();
  return collect_metrics();
}

}  // namespace saige::crowd
