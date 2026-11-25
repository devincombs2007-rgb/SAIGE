#include "saige/navigation.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <queue>
#include <stdexcept>
#include <unordered_map>

namespace saige {

namespace {

bool fin(float x) { return std::isfinite(x); }
constexpr std::int64_t kNoNode = INT64_MIN;  // negative lattice indices make negative keys, so -1 cannot mean none
std::int64_t key(std::int32_t ix, std::int32_t iy) { return (static_cast<std::int64_t>(ix) << 32) | static_cast<std::uint32_t>(iy); }
void validate(const NavConfig& c) {
    if (!(c.cell >= 0.1f && c.cell <= 4.0f) || !(c.agent_radius > 0.0f && c.agent_radius < 2.0f) || !(c.agent_height >= 2.0f * c.agent_radius && c.agent_height < 5.0f) || !(c.max_step >= 0.0f && c.max_step < 2.0f) ||
        !(c.max_slope > 0.0f && c.max_slope < 1.4f) || !(c.search_top > 0.0f && c.search_top < 100.0f) || !(c.search_depth > 0.0f && c.search_depth < 200.0f) || !fin(c.cell) || !fin(c.agent_radius) ||
        !fin(c.agent_height) || !fin(c.max_step) || !fin(c.max_slope) || !fin(c.search_top) || !fin(c.search_depth)) {
        throw std::invalid_argument("NavConfig: a value is out of range or not finite");
    }
}

} // namespace

NavGrid build_nav_grid(const StaticCollision& world, QueryScratch& scratch, const NavConfig& config, float min_x, float min_y, float max_x, float max_y, float floor_z) {
    validate(config);
    if (!fin(min_x) || !fin(min_y) || !fin(max_x) || !fin(max_y) || !fin(floor_z) || !(max_x >= min_x) || !(max_y >= min_y)) throw std::invalid_argument("build_nav_grid: the region must be finite and not empty");
    if (std::fabs(min_x) > max_local_extent || std::fabs(max_x) > max_local_extent || std::fabs(min_y) > max_local_extent || std::fabs(max_y) > max_local_extent) throw std::invalid_argument("build_nav_grid: the region is outside the physics envelope");
    NavGrid g;
    g.cell = config.cell;
    g.ix0 = static_cast<std::int32_t>(std::ceil(min_x / config.cell - 1e-4f));
    g.iy0 = static_cast<std::int32_t>(std::ceil(min_y / config.cell - 1e-4f));
    const std::int32_t ix1 = static_cast<std::int32_t>(std::floor(max_x / config.cell + 1e-4f)), iy1 = static_cast<std::int32_t>(std::floor(max_y / config.cell + 1e-4f));
    g.nx = std::max(0, ix1 - g.ix0 + 1);
    g.ny = std::max(0, iy1 - g.iy0 + 1);
    if (g.nx > 4096 || g.ny > 4096) throw std::invalid_argument("build_nav_grid: more than 4096 points on a side");
    g.ground.assign(static_cast<std::size_t>(g.nx) * static_cast<std::size_t>(g.ny), 0.0f);
    g.walkable.assign(g.ground.size(), 0);
    const float min_up = std::cos(config.max_slope);
    for (std::int32_t iy = g.iy0; iy < g.iy0 + g.ny; ++iy) {
        for (std::int32_t ix = g.ix0; ix < g.ix0 + g.nx; ++ix) {
            const float x = static_cast<float>(ix) * config.cell, y = static_cast<float>(iy) * config.cell;
            const auto hit = world.raycast(Vec3{x, y, floor_z + config.search_top}, Vec3{0, 0, -1}, config.search_top + config.search_depth, RayMode::Surface);
            if (!hit || hit->normal.z < min_up) continue;
            const float z = hit->point.z;
            // Clearance is checked from the step height up: a curb the agent walks over is not an obstacle to it.
            const Capsule c{Vec3{x, y, z + config.max_step + 0.02f}, config.agent_radius, std::max(2.0f * config.agent_radius, config.agent_height - config.max_step - 0.02f)};
            if (world.overlaps_capsule(c, scratch)) continue;
            const std::size_t i = g.index(ix, iy);
            g.ground[i] = z;
            g.walkable[i] = 1;
        }
    }
    return g;
}

NavMap::NavMap(const NavConfig& config) : config_(config) { validate(config_); }

void NavMap::set_chunk(std::uint32_t id, NavGrid grid) {
    if (!(std::fabs(grid.cell - config_.cell) < 1e-6f) || grid.ground.size() != static_cast<std::size_t>(grid.nx) * static_cast<std::size_t>(grid.ny) || grid.walkable.size() != grid.ground.size()) {
        throw std::invalid_argument("NavMap::set_chunk: the grid does not match this map (cell size or storage)");
    }
    chunks_[id] = std::move(grid);
}

void NavMap::remove_chunk(std::uint32_t id) { chunks_.erase(id); }

const NavGrid* NavMap::grid_for(std::int32_t ix, std::int32_t iy) const {
    for (const auto& entry : chunks_) {
        if (entry.second.contains(ix, iy)) return &entry.second;
    }
    return nullptr;
}

bool NavMap::walkable_at(float x, float y, float* ground) const {
    const std::int32_t ix = static_cast<std::int32_t>(std::lround(x / config_.cell)), iy = static_cast<std::int32_t>(std::lround(y / config_.cell));
    const NavGrid* g = grid_for(ix, iy);
    if (g == nullptr || !g->walkable[g->index(ix, iy)]) return false;
    if (ground != nullptr) *ground = g->ground[g->index(ix, iy)];
    return true;
}

bool NavMap::nearest_walkable(float x, float y, float radius, NavPoint& out) const {
    const std::int32_t cx = static_cast<std::int32_t>(std::lround(x / config_.cell)), cy = static_cast<std::int32_t>(std::lround(y / config_.cell));
    const std::int32_t rings = static_cast<std::int32_t>(std::ceil(radius / config_.cell));
    float best = radius * radius + 1e-6f;
    bool found = false;
    for (std::int32_t dy = -rings; dy <= rings; ++dy) {
        for (std::int32_t dx = -rings; dx <= rings; ++dx) {
            const std::int32_t ix = cx + dx, iy = cy + dy;
            const NavGrid* g = grid_for(ix, iy);
            if (g == nullptr || !g->walkable[g->index(ix, iy)]) continue;
            const float px = static_cast<float>(ix) * config_.cell, py = static_cast<float>(iy) * config_.cell;
            const float d2 = (px - x) * (px - x) + (py - y) * (py - y);
            if (d2 < best) {  // the first of equals in scan order wins: deterministic
                best = d2;
                out.position = {px, py, g->ground[g->index(ix, iy)]};
                found = true;
            }
        }
    }
    return found;
}

bool NavMap::step_ok(std::int32_t ix, std::int32_t iy, std::int32_t jx, std::int32_t jy) const {
    const NavGrid* a = grid_for(ix, iy);
    const NavGrid* b = grid_for(jx, jy);
    if (a == nullptr || b == nullptr || !a->walkable[a->index(ix, iy)] || !b->walkable[b->index(jx, jy)]) return false;
    return std::fabs(a->ground[a->index(ix, iy)] - b->ground[b->index(jx, jy)]) <= config_.max_step;
}

bool NavMap::line_ok(const NavPoint& a, const NavPoint& b) const {
    const float dx = b.position.x - a.position.x, dy = b.position.y - a.position.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    const int samples = std::max(1, static_cast<int>(std::ceil(len / (config_.cell * 0.5f))));
    float last_z = a.position.z;
    for (int s = 0; s <= samples; ++s) {
        const float t = static_cast<float>(s) / static_cast<float>(samples);
        const float x = a.position.x + dx * t, y = a.position.y + dy * t;
        // The four lattice points around the sample must all be walkable (a margin of a cell: the lattice is the clearance), and the ground must not step more than max_step along the way.
        const std::int32_t fx = static_cast<std::int32_t>(std::floor(x / config_.cell)), fy = static_cast<std::int32_t>(std::floor(y / config_.cell));
        float zsum = 0.0f;
        for (std::int32_t oy = 0; oy <= 1; ++oy) {
            for (std::int32_t ox = 0; ox <= 1; ++ox) {
                float z;
                if (!walkable_at(static_cast<float>(fx + ox) * config_.cell, static_cast<float>(fy + oy) * config_.cell, &z)) return false;
                zsum += z;
            }
        }
        const float z = zsum * 0.25f;
        if (std::fabs(z - last_z) > config_.max_step) return false;
        last_z = z;
    }
    return true;
}

std::vector<NavPoint> NavMap::find_path(Vec3 from, Vec3 to, std::size_t max_nodes) const {
    if (!fin(from.x) || !fin(from.y) || !fin(from.z) || !fin(to.x) || !fin(to.y) || !fin(to.z)) throw std::invalid_argument("NavMap::find_path: non-finite endpoint");
    NavPoint s, g;
    if (!nearest_walkable(from.x, from.y, 1.0f, s) || !nearest_walkable(to.x, to.y, 1.0f, g)) return {};
    const std::int32_t sx = static_cast<std::int32_t>(std::lround(s.position.x / config_.cell)), sy = static_cast<std::int32_t>(std::lround(s.position.y / config_.cell));
    const std::int32_t gx = static_cast<std::int32_t>(std::lround(g.position.x / config_.cell)), gy = static_cast<std::int32_t>(std::lround(g.position.y / config_.cell));
    if (sx == gx && sy == gy) return {s, g};
    struct Node {
        float g;
        std::int64_t parent;
        bool closed;
    };
    std::unordered_map<std::int64_t, Node> nodes;
    struct Open {
        float f;
        std::int64_t k;
        bool operator>(const Open& o) const { return f != o.f ? f > o.f : k > o.k; }
    };
    std::priority_queue<Open, std::vector<Open>, std::greater<Open>> open;
    const auto heuristic = [&](std::int32_t x, std::int32_t y) {
        const float dx = static_cast<float>(std::abs(x - gx)), dy = static_cast<float>(std::abs(y - gy));
        return config_.cell * ((dx + dy) + (1.41421356f - 2.0f) * std::min(dx, dy));
    };
    nodes[key(sx, sy)] = {0.0f, kNoNode, false};
    open.push({heuristic(sx, sy), key(sx, sy)});
    std::size_t expanded = 0;
    std::int64_t goal_key = kNoNode;
    while (!open.empty()) {
        const Open top = open.top();
        open.pop();
        Node& n = nodes[top.k];
        if (n.closed) continue;
        n.closed = true;
        const std::int32_t cx = static_cast<std::int32_t>(top.k >> 32), cy = static_cast<std::int32_t>(static_cast<std::uint32_t>(top.k & 0xffffffff));
        if (cx == gx && cy == gy) {
            goal_key = top.k;
            break;
        }
        if (++expanded > max_nodes) return {};
        const float base = n.g;
        for (std::int32_t dy = -1; dy <= 1; ++dy) {
            for (std::int32_t dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const std::int32_t nx = cx + dx, ny = cy + dy;
                if (!step_ok(cx, cy, nx, ny)) continue;
                if (dx != 0 && dy != 0 && (!step_ok(cx, cy, cx + dx, cy) || !step_ok(cx, cy, cx, cy + dy))) continue;  // no cutting a corner
                const float cost = base + config_.cell * (dx != 0 && dy != 0 ? 1.41421356f : 1.0f);
                auto it = nodes.find(key(nx, ny));
                if (it != nodes.end() && (it->second.closed || it->second.g <= cost)) continue;
                nodes[key(nx, ny)] = {cost, top.k, false};
                open.push({cost + heuristic(nx, ny), key(nx, ny)});
            }
        }
    }
    if (goal_key == kNoNode) return {};
    std::vector<NavPoint> raw;
    for (std::int64_t k = goal_key; k != kNoNode; k = nodes[k].parent) {
        const std::int32_t ix = static_cast<std::int32_t>(k >> 32), iy = static_cast<std::int32_t>(static_cast<std::uint32_t>(k & 0xffffffff));
        float z = 0.0f;
        (void)walkable_at(static_cast<float>(ix) * config_.cell, static_cast<float>(iy) * config_.cell, &z);
        raw.push_back({{static_cast<float>(ix) * config_.cell, static_cast<float>(iy) * config_.cell, z}});
    }
    std::reverse(raw.begin(), raw.end());
    // String pulling: from each point take the furthest later point that can be walked to in a straight line.
    std::vector<NavPoint> path{raw.front()};
    std::size_t i = 0;
    while (i + 1 < raw.size()) {
        std::size_t j = raw.size() - 1;
        while (j > i + 1 && !line_ok(raw[i], raw[j])) --j;
        path.push_back(raw[j]);
        i = j;
    }
    return path;
}

bool pedestrian_go(const NavMap& map, PedestrianAgent& agent, Vec3 goal) {
    agent.path = map.find_path(agent.position, goal);
    agent.arrived = false;
    agent.failed = agent.path.empty();
    agent.next = agent.path.size() > 1 ? 1 : 0;
    return !agent.failed;
}

void pedestrian_step(const NavMap& map, const PedestrianConfig& c, PedestrianAgent& a, const std::vector<PedestrianNeighbor>& others, float dt) {
    if (!(dt > 0.0f) || !fin(dt)) throw std::invalid_argument("pedestrian_step: dt must be finite and positive");
    float desired_x = 0.0f, desired_y = 0.0f;
    if (!a.arrived && !a.failed && !a.path.empty()) {
        // Passing waypoints (the last one is the goal and is reached by the arrival radius).
        while (a.next + 1 < a.path.size()) {
            const NavPoint& w = a.path[a.next];
            if (std::hypot(w.position.x - a.position.x, w.position.y - a.position.y) < c.waypoint_radius) ++a.next;
            else break;
        }
        const NavPoint& w = a.path[std::min(a.next, a.path.size() - 1)];
        const float dx = w.position.x - a.position.x, dy = w.position.y - a.position.y;
        const float d = std::hypot(dx, dy);
        const bool last = a.next + 1 >= a.path.size();
        if (last && d < c.arrive_radius) {
            a.arrived = true;
        } else if (d > 1e-5f) {
            const float speed = last ? c.walk_speed * std::min(1.0f, d / (2.0f * c.arrive_radius + 0.5f)) : c.walk_speed;
            desired_x = dx / d * speed;
            desired_y = dy / d * speed;
        }
    }
    // Keep away from the others: a push away that grows as they get closer than `separation` sums of radii, and a sidestep to the right for those on course to pass too close.
    const float limit = c.separation * 2.0f * c.radius;
    const float heading_x = (a.velocity.x != 0.0f || a.velocity.y != 0.0f) ? a.velocity.x : desired_x, heading_y = (a.velocity.x != 0.0f || a.velocity.y != 0.0f) ? a.velocity.y : desired_y;
    const float hl = std::hypot(heading_x, heading_y);
    for (const PedestrianNeighbor& o : others) {
        const float dx = a.position.x - o.position.x, dy = a.position.y - o.position.y;
        const float d = std::hypot(dx, dy);
        if (d < 1e-6f) continue;
        if (d < limit) {
            const float push = (1.0f - d / limit) * c.separation_weight * c.walk_speed;
            desired_x += dx / d * push;
            desired_y += dy / d * push;
        }
        // Closest approach of the neighbour relative to this agent, within the horizon.
        const float rx = o.position.x - a.position.x, ry = o.position.y - a.position.y;
        const float vx = o.velocity.x - a.velocity.x, vy = o.velocity.y - a.velocity.y;
        const float v2 = vx * vx + vy * vy;
        if (v2 < 1e-6f || hl < 1e-4f) continue;
        const float tca = std::clamp(-(rx * vx + ry * vy) / v2, 0.0f, c.horizon);
        const float cx = rx + vx * tca, cy = ry + vy * tca;
        if (std::hypot(cx, cy) < 2.0f * c.radius * 1.3f && tca > 0.0f) {
            const float urgency = 1.0f - tca / c.horizon;
            desired_x += heading_y / hl * c.avoid_weight * c.walk_speed * urgency;   // to the right of the heading: (hy, -hx)
            desired_y += -heading_x / hl * c.avoid_weight * c.walk_speed * urgency;
        }
    }
    const float dl = std::hypot(desired_x, desired_y);
    if (dl > c.walk_speed) {
        desired_x *= c.walk_speed / dl;
        desired_y *= c.walk_speed / dl;
    }
    // Accelerate toward it.
    const float ax = desired_x - a.velocity.x, ay = desired_y - a.velocity.y;
    const float al = std::hypot(ax, ay);
    const float max_dv = c.acceleration * dt;
    if (al > max_dv) {
        a.velocity.x += ax / al * max_dv;
        a.velocity.y += ay / al * max_dv;
    } else {
        a.velocity.x = desired_x;
        a.velocity.y = desired_y;
    }
    // Move, staying on walkable ground: a step into an unwalkable lattice cell is slid along one axis or dropped.
    const float nx = a.position.x + a.velocity.x * dt, ny = a.position.y + a.velocity.y * dt;
    float z = a.position.z;
    if (map.walkable_at(nx, ny, &z)) {
        a.position = {nx, ny, z};
    } else if (map.walkable_at(nx, a.position.y, &z)) {
        a.position = {nx, a.position.y, z};
        a.velocity.y = 0.0f;
    } else if (map.walkable_at(a.position.x, ny, &z)) {
        a.position = {a.position.x, ny, z};
        a.velocity.x = 0.0f;
    } else {
        a.velocity = {0, 0, 0};
    }
}

} // namespace saige
