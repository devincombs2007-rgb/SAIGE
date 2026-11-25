#include "saige/multi_floor_nav.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>

namespace saige::multi_floor_nav {

namespace {

[[nodiscard]] bool node_less(const NavNodeId& a, const NavNodeId& b) noexcept {
  if (a.gx != b.gx) return a.gx < b.gx;
  if (a.gy != b.gy) return a.gy < b.gy;
  return a.layer < b.layer;
}

[[nodiscard]] double heuristic_cost(const FloorSlice& a, const FloorSlice& b,
                                    double cell_size_m) noexcept {
  const double dx = std::abs(static_cast<double>(a.id.gx) -
                             static_cast<double>(b.id.gx));
  const double dy = std::abs(static_cast<double>(a.id.gy) -
                             static_cast<double>(b.id.gy));
  const double dmin = std::min(dx, dy);
  const double dmax = std::max(dx, dy);
  const double horiz = (dmax - dmin + std::sqrt(2.0) * dmin) * cell_size_m;
  const double vert = std::abs(a.z_m - b.z_m);
  return horiz + vert;
}

}  // namespace

void PortalTraversalProgress::begin(PortalKind kind, NavNodeId from,
                                    NavNodeId to, double z_from, double z_to,
                                    double duration_s) noexcept {
  from_node = from;
  to_node = to;
  from_z_m = z_from;
  to_z_m = z_to;
  duration_sec = (std::isfinite(duration_s) && duration_s > 0.0) ? duration_s : 0.1;
  elapsed_sec = 0.0;
  switch (kind) {
    case PortalKind::Stair:
      state = PortalTraversalState::TraversingStair;
      break;
    case PortalKind::Ramp:
      state = PortalTraversalState::TraversingRamp;
      break;
    case PortalKind::Elevator:
      state = PortalTraversalState::UsingElevator;
      break;
    case PortalKind::InteriorDoorway:
      state = PortalTraversalState::PassingDoorway;
      break;
  }
}

void PortalTraversalProgress::step(double dt_seconds) noexcept {
  if (!is_active() || !std::isfinite(dt_seconds) || dt_seconds <= 0.0) {
    return;
  }
  elapsed_sec = std::min(duration_sec, elapsed_sec + dt_seconds);
  if (elapsed_sec >= duration_sec) {
    state = PortalTraversalState::OnFloor;
  }
}

bool PortalTraversalProgress::is_active() const noexcept {
  return state != PortalTraversalState::OnFloor;
}

bool PortalTraversalProgress::is_complete() const noexcept {
  return state == PortalTraversalState::OnFloor && elapsed_sec >= duration_sec &&
         duration_sec > 0.0;
}

double PortalTraversalProgress::normalized_progress() const noexcept {
  if (duration_sec <= 0.0) return 1.0;
  return std::clamp(elapsed_sec / duration_sec, 0.0, 1.0);
}

double PortalTraversalProgress::current_z_m() const noexcept {
  const double t = normalized_progress();
  // Smoothstep for natural character controller vertical motion
  const double s = t * t * (3.0 - 2.0 * t);
  return from_z_m + s * (to_z_m - from_z_m);
}

const NavPortalLink* MultiFloorNavGraph::find_portal_between(
    NavNodeId a, NavNodeId b) const noexcept {
  for (const auto& portal : portals_) {
    if (portal.from == a && portal.to == b) return &portal;
    if (portal.bidirectional && portal.from == b && portal.to == a) {
      return &portal;
    }
  }
  return nullptr;
}

MultiFloorNavGraph::MultiFloorNavGraph(double cell_size_m,
                                       double max_step_height_m)
    : cell_size_m_(cell_size_m > 0.05 ? cell_size_m : 0.5),
      max_step_height_m_(max_step_height_m >= 0.0 ? max_step_height_m : 0.35) {}

bool MultiFloorNavGraph::set_slice(const FloorSlice& slice) {
  if (!std::isfinite(slice.z_m)) return false;
  for (auto& s : slices_) {
    if (s.id == slice.id) {
      s = slice;
      return true;
    }
  }
  slices_.push_back(slice);
  std::sort(slices_.begin(), slices_.end(),
            [](const FloorSlice& a, const FloorSlice& b) {
              return node_less(a.id, b.id);
            });
  return true;
}

const FloorSlice* MultiFloorNavGraph::find_slice(NavNodeId id) const noexcept {
  for (const auto& s : slices_) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

bool MultiFloorNavGraph::add_portal(const NavPortalLink& portal) {
  if (portal.id == 0 || !std::isfinite(portal.traversal_cost) ||
      portal.traversal_cost <= 0.0) {
    return false;
  }
  for (const auto& p : portals_) {
    if (p.id == portal.id) return false;
  }
  portals_.push_back(portal);
  std::sort(portals_.begin(), portals_.end(),
            [](const NavPortalLink& a, const NavPortalLink& b) {
              return a.id < b.id;
            });
  return true;
}

MultiFloorPathResult MultiFloorNavGraph::find_path(NavNodeId start,
                                                   NavNodeId goal) const {
  MultiFloorPathResult result{};
  const FloorSlice* start_slice = find_slice(start);
  const FloorSlice* goal_slice = find_slice(goal);
  if (start_slice == nullptr || goal_slice == nullptr ||
      !start_slice->walkable || !goal_slice->walkable) {
    return result;
  }

  auto index_of = [&](NavNodeId id) -> std::ptrdiff_t {
    for (std::size_t i = 0; i < slices_.size(); ++i) {
      if (slices_[i].id == id) return static_cast<std::ptrdiff_t>(i);
    }
    return -1;
  };

  const std::ptrdiff_t start_idx = index_of(start);
  const std::ptrdiff_t goal_idx = index_of(goal);
  if (start_idx < 0 || goal_idx < 0) return result;

  // Defect 2 fix: if any portal can shortcut below geometric distance,
  // geometric h() is inadmissible; use h = 0.0 (optimal Dijkstra) to guarantee
  // shortest-cost paths.
  bool has_shortcut_portal = false;
  for (const auto& p : portals_) {
    const FloorSlice* from_s = find_slice(p.from);
    const FloorSlice* to_s = find_slice(p.to);
    if (from_s != nullptr && to_s != nullptr) {
      if (p.traversal_cost + 1e-9 <
          heuristic_cost(*from_s, *to_s, cell_size_m_)) {
        has_shortcut_portal = true;
        break;
      }
    }
  }

  auto eval_h = [&](const FloorSlice& s) -> double {
    return has_shortcut_portal ? 0.0
                               : heuristic_cost(s, *goal_slice, cell_size_m_);
  };

  const std::size_t n = slices_.size();
  constexpr double kInf = std::numeric_limits<double>::infinity();
  std::vector<double> g_score(n, kInf);
  std::vector<std::ptrdiff_t> parent(n, -1);
  std::vector<bool> closed(n, false);

  struct QueueEntry {
    double f = 0.0;
    std::size_t idx = 0;
    bool operator>(const QueueEntry& other) const noexcept {
      if (std::abs(f - other.f) > 1e-9) return f > other.f;
      return idx > other.idx;
    }
  };

  std::priority_queue<QueueEntry, std::vector<QueueEntry>, std::greater<QueueEntry>>
      open;

  const auto u_start = static_cast<std::size_t>(start_idx);
  const auto u_goal = static_cast<std::size_t>(goal_idx);
  g_score[u_start] = 0.0;
  open.push({eval_h(*start_slice), u_start});

  static constexpr int kDx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
  static constexpr int kDy[8] = {0, 0, 1, -1, 1, -1, 1, -1};

  auto is_passable_neighbor = [&](std::int32_t gx, std::int32_t gy,
                                  std::uint8_t layer, double ref_z) -> bool {
    const FloorSlice* s = find_slice({gx, gy, layer});
    return s != nullptr && s->walkable &&
           std::abs(s->z_m - ref_z) <= max_step_height_m_;
  };

  while (!open.empty()) {
    const std::size_t cur = open.top().idx;
    open.pop();
    if (closed[cur]) continue;
    closed[cur] = true;

    if (cur == u_goal) {
      result.found = true;
      result.total_cost = g_score[cur];
      std::vector<NavNodeId> rev;
      for (std::ptrdiff_t p = static_cast<std::ptrdiff_t>(cur); p >= 0;
           p = parent[static_cast<std::size_t>(p)]) {
        const auto& sl = slices_[static_cast<std::size_t>(p)];
        rev.push_back(sl.id);
        if (sl.is_interior) ++result.interior_steps;
      }
      std::reverse(rev.begin(), rev.end());
      for (std::size_t i = 1; i < rev.size(); ++i) {
        if (rev[i].layer != rev[i - 1].layer) ++result.portal_transitions;
      }
      result.waypoints = std::move(rev);
      return result;
    }

    const FloorSlice& cur_slice = slices_[cur];

    for (int d = 0; d < 8; ++d) {
      // Defect 3 fix: use int64_t arithmetic and bounds check to prevent INT32 overflow
      const std::int64_t nx64 =
          static_cast<std::int64_t>(cur_slice.id.gx) + kDx[d];
      const std::int64_t ny64 =
          static_cast<std::int64_t>(cur_slice.id.gy) + kDy[d];
      if (nx64 < std::numeric_limits<std::int32_t>::min() ||
          nx64 > std::numeric_limits<std::int32_t>::max() ||
          ny64 < std::numeric_limits<std::int32_t>::min() ||
          ny64 > std::numeric_limits<std::int32_t>::max()) {
        continue;
      }
      const auto nx = static_cast<std::int32_t>(nx64);
      const auto ny = static_cast<std::int32_t>(ny64);

      // Defect 1 fix: forbid diagonal corner cutting across blocked/absent cardinal cells
      if (d >= 4) {
        if (!is_passable_neighbor(nx, cur_slice.id.gy, cur_slice.id.layer,
                                  cur_slice.z_m) ||
            !is_passable_neighbor(cur_slice.id.gx, ny, cur_slice.id.layer,
                                  cur_slice.z_m)) {
          continue;
        }
      }

      const std::ptrdiff_t j_signed =
          index_of({nx, ny, cur_slice.id.layer});
      if (j_signed < 0) continue;
      const auto j = static_cast<std::size_t>(j_signed);
      const FloorSlice& nb = slices_[j];
      if (!nb.walkable ||
          std::abs(nb.z_m - cur_slice.z_m) > max_step_height_m_) {
        continue;
      }

      const double step_dist =
          (d < 4 ? 1.0 : std::sqrt(2.0)) * cell_size_m_;
      const double dz = std::abs(nb.z_m - cur_slice.z_m);
      const double tentative = g_score[cur] + step_dist + dz;
      if (tentative < g_score[j]) {
        g_score[j] = tentative;
        parent[j] = static_cast<std::ptrdiff_t>(cur);
        open.push({tentative + eval_h(nb), j});
      }
    }

    for (const auto& portal : portals_) {
      NavNodeId target_id{};
      if (portal.from == cur_slice.id) {
        target_id = portal.to;
      } else if (portal.bidirectional && portal.to == cur_slice.id) {
        target_id = portal.from;
      } else {
        continue;
      }
      const std::ptrdiff_t j_signed = index_of(target_id);
      if (j_signed < 0) continue;
      const auto j = static_cast<std::size_t>(j_signed);
      if (!slices_[j].walkable) continue;

      const double tentative = g_score[cur] + portal.traversal_cost;
      if (tentative < g_score[j]) {
        g_score[j] = tentative;
        parent[j] = static_cast<std::size_t>(cur);
        open.push({tentative + eval_h(slices_[j]), j});
      }
    }
  }

  return result;
}

}  // namespace saige::multi_floor_nav
