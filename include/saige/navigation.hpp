#pragma once

// Navigation over the static collision, in pieces that stream: a walkability grid per world chunk, a map that joins the chunks that are loaded, an A* path search across them, and pedestrian agents that follow a path
// and keep out of each other's way (issue #43, world simulation). Deterministic (no randomness, ties broken by cell index), allocation-bounded per query, independent of rendering and of the character controller.
//
// A grid samples the collision on a lattice of `cell` meters: for each lattice point a ray from above finds the ground (the first surface below `search_top` whose normal is within `max_slope` of up), and the
// point is walkable when a standing capsule of the agent's size at that ground does not overlap anything. All grids of one map share the lattice (their origins are whole numbers of cells in one frame), so
// neighbouring chunks line up exactly and a path crosses the border. A step between two neighbouring lattice points needs both walkable and their ground heights within `max_step`; a diagonal also needs the two
// cells beside it (no cutting a corner). Paths are shortened by string pulling over the same walkability (a straight run is taken only if every lattice point along it is walkable and its steps are small).
//
// Frame and range: one local frame in meters, within the physics envelope (+-max_local_extent, capsule_sweep.hpp); a host streaming a larger world rebases (migrate_origin) and rebuilds or shifts its grids.
// Off-mesh links (NavLink: a jump across a gap, a ladder, a door, a lift) join two places that the lattice does not: A* may cross one for its length plus an extra cost, and a path says which of its segments are links
// (`NavPoint::link`). Dynamic obstacles (blockers: a circle in the plane) make the lattice points near them unwalkable until they are cleared, without rebuilding any grid; `path_clear` says whether an existing path
// still holds and `pedestrian_replan` goes around what has appeared.
// What this is not: no navigation mesh (a lattice, so the narrowest passage it finds is one cell wide), no multiple floors in one column (the highest walkable surface below `search_top` is the one used: a bridge
// over a road is two grids of two maps joined by links, see NavWorld), no navigation of the vertical (a link is the only way to leave the ground).

#include "saige/static_collision.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace saige {

struct NavConfig {
    float cell = 0.5f;              // lattice spacing, m
    float agent_radius = 0.3f;
    float agent_height = 1.7f;
    float max_step = 0.35f;         // the tallest step between neighbouring points
    float max_slope = 0.7f;         // rad from level
    float search_top = 4.0f;        // m above `floor_z` where the ground search starts
    float search_depth = 8.0f;      // how far down it looks
};

struct NavGrid {
    std::int32_t ix0 = 0, iy0 = 0;  // lattice index of the first point (the point is at ix * cell)
    std::int32_t nx = 0, ny = 0;
    float cell = 0.5f;
    std::vector<float> ground;      // nx * ny heights (meaningful where walkable)
    std::vector<std::uint8_t> walkable;
    [[nodiscard]] std::size_t index(std::int32_t ix, std::int32_t iy) const noexcept { return static_cast<std::size_t>(iy - iy0) * static_cast<std::size_t>(nx) + static_cast<std::size_t>(ix - ix0); }
    [[nodiscard]] bool contains(std::int32_t ix, std::int32_t iy) const noexcept { return ix >= ix0 && iy >= iy0 && ix < ix0 + nx && iy < iy0 + ny; }
};

// Samples the lattice points whose coordinates lie in [min_x, max_x] x [min_y, max_y]. `floor_z` is where the ground search is anchored (a point's search starts at floor_z + search_top). Throws
// std::invalid_argument for a non-finite or empty region, an invalid configuration, a region more than 4096 points on a side, or one outside the physics envelope.
[[nodiscard]] NavGrid build_nav_grid(const StaticCollision& world, QueryScratch& scratch, const NavConfig& config, float min_x, float min_y, float max_x, float max_y, float floor_z = 0.0f);

struct NavPoint {
    Vec3 position;                  // on the ground
    bool link = false;              // the segment from this point to the next is an off-mesh link: traverse it, do not walk it
};

// An off-mesh link between two places (each moved to its nearest walkable lattice point within a metre when a path is searched). `extra_cost` is added to its length, so a link is taken only when it saves more than that.
struct NavLink {
    Vec3 from, to;
    float extra_cost = 0.0f;
    bool bidirectional = true;
};

// A dynamic obstacle: lattice points within `radius + agent_radius` of the centre are unwalkable while it exists.
struct NavBlocker {
    float x = 0.0f, y = 0.0f, radius = 0.5f;
};

class NavMap {
public:
    explicit NavMap(const NavConfig& config);
    // Adds or replaces the grid of a chunk (by id). Throws std::invalid_argument for a grid whose cell is not this map's.
    void set_chunk(std::uint32_t id, NavGrid grid);
    // Removes a chunk (it streamed out): paths through it stop existing.
    void remove_chunk(std::uint32_t id);
    [[nodiscard]] std::size_t chunk_count() const noexcept { return chunks_.size(); }
    // Whether the lattice point nearest to (x, y) is walkable, and its ground height.
    [[nodiscard]] bool walkable_at(float x, float y, float* ground = nullptr) const;
    // The nearest walkable lattice point to (x, y) within `radius` meters (search by rings); false when none.
    [[nodiscard]] bool nearest_walkable(float x, float y, float radius, NavPoint& out) const;
    // A path from `from` to `to` (each moved to its nearest walkable lattice point within one metre): the waypoints, shortened by string pulling, first and last being those lattice points. Empty when there is no path
    // (an end is not near walkable ground, the chunks between are not loaded, an obstacle encloses one). `max_nodes` bounds the search: a search that would expand more gives up (empty).
    [[nodiscard]] std::vector<NavPoint> find_path(Vec3 from, Vec3 to, std::size_t max_nodes = 200000) const;
    [[nodiscard]] const NavConfig& config() const noexcept { return config_; }

    // Off-mesh links by id (a second add of an id replaces it). Throws std::invalid_argument for non-finite ends or a negative or non-finite extra cost.
    void set_link(std::uint32_t id, const NavLink& link);
    void remove_link(std::uint32_t id) { links_.erase(id); }
    [[nodiscard]] std::size_t link_count() const noexcept { return links_.size(); }
    // Dynamic obstacles by id (same replacement rule). Throws std::invalid_argument for a non-finite centre or a radius that is not positive and finite.
    void set_blocker(std::uint32_t id, const NavBlocker& blocker);
    void remove_blocker(std::uint32_t id) { blockers_.erase(id); }
    [[nodiscard]] std::size_t blocker_count() const noexcept { return blockers_.size(); }
    // Whether every walked segment of `path` still holds (the lattice points along it are walkable and not blocked, steps small) and every link's ends are walkable: false once a blocker has appeared on it.
    [[nodiscard]] bool path_clear(const std::vector<NavPoint>& path) const;

private:
    [[nodiscard]] bool blocked(std::int32_t ix, std::int32_t iy) const;
    [[nodiscard]] const NavGrid* grid_for(std::int32_t ix, std::int32_t iy) const;
    [[nodiscard]] bool step_ok(std::int32_t ix, std::int32_t iy, std::int32_t jx, std::int32_t jy) const;
    [[nodiscard]] bool line_ok(const NavPoint& a, const NavPoint& b) const;
    NavConfig config_;
    std::map<std::uint32_t, NavLink> links_;
    std::map<std::uint32_t, NavBlocker> blockers_;
    std::map<std::uint32_t, NavGrid> chunks_;  // ordered: a lookup that two grids could both answer is answered the same way on every machine
};

struct PedestrianConfig {
    float radius = 0.28f;
    float walk_speed = 1.4f;
    float acceleration = 3.0f;      // m/s^2
    float arrive_radius = 0.25f;    // the goal is reached within this
    float waypoint_radius = 0.35f;  // a waypoint is passed within this
    float separation = 1.4f;        // other agents are pushed away inside this many sums of radii
    float separation_weight = 1.6f;
    float horizon = 2.5f;           // s: a neighbour that will pass closer than the sum of radii (times 1.3) within this time is steered around, to the right
    float avoid_weight = 2.5f;
};

struct PedestrianNeighbor {
    Vec3 position;
    Vec3 velocity;
};

struct PedestrianAgent {
    Vec3 position;
    Vec3 velocity;
    std::vector<NavPoint> path;
    std::size_t next = 0;           // index of the waypoint being walked to
    bool arrived = false;
    bool failed = false;            // no path was found
};

// Gives an agent a path to `goal` (from its position). Returns false (and marks it failed) when there is none.
bool pedestrian_go(const NavMap& map, PedestrianAgent& agent, Vec3 goal);
// If the rest of the agent's path (from the waypoint it is walking to) no longer holds, a new path to `goal` is searched. Returns true when the agent has a path to follow afterwards (the old one if it still held).
bool pedestrian_replan(const NavMap& map, PedestrianAgent& agent, Vec3 goal);
// One step of dt seconds: steer toward the next waypoint at walk speed, apart from close neighbours and around those on course to collide (the closest approach of the relative motion within `horizon`; the sidestep
// is always to the right of the way the agent is heading, so two that meet head on each give way to the same side and a lane forms), accelerate within the limit, and stay on the grid's ground (the height
// follows the nearest walkable point). Deterministic. `others` may include the agent itself (it is skipped by being at distance 0 from itself).
void pedestrian_step(const NavMap& map, const PedestrianConfig& config, PedestrianAgent& agent, const std::vector<PedestrianNeighbor>& others, float dt);

} // namespace saige

namespace saige {

// A crowd stepped together: every agent sees only the neighbours that can matter (those within the distance at which the separation push or the predicted-collision sidestep can act, found with a uniform grid), in index
// order, so the result is bit-identical to calling pedestrian_step for each agent against all the others (checked by the tests) at a cost that grows with the density, not with the square of the population. Agents are updated from
// the state at the start of the step (not one after another), so the order of the agents does not matter. Throws what pedestrian_step throws.
void crowd_step(const NavMap& map, const PedestrianConfig& config, std::vector<PedestrianAgent>& agents, float dt);

} // namespace saige
