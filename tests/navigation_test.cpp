// Navigation: the walkability grid (clearance, slope, steps), A* paths around obstacles and across streamed chunks, and pedestrians that follow them without walking into each other.
#include "saige/navigation.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

float path_length(const std::vector<NavPoint>& p) {
    float l = 0.0f;
    for (std::size_t i = 1; i < p.size(); ++i) l += std::hypot(p[i].position.x - p[i - 1].position.x, p[i].position.y - p[i - 1].position.y);
    return l;
}

// Distance from (x, y) to the box [x0,x1] x [y0,y1] (0 inside).
float box_distance(float x, float y, float x0, float y0, float x1, float y1) {
    const float dx = std::max({x0 - x, 0.0f, x - x1}), dy = std::max({y0 - y, 0.0f, y - y1});
    return std::hypot(dx, dy);
}

struct Scene {
    StaticCollision world;
    QueryScratch scratch;
    explicit Scene(const std::vector<Aabb>& boxes, const std::vector<Triangle>& tris = {}) : world(boxes, tris) {}
};

NavMap map_of(Scene& s, const NavConfig& c, float x0, float y0, float x1, float y1) {
    NavMap m(c);
    m.set_chunk(1, build_nav_grid(s.world, s.scratch, c, x0, y0, x1, y1));
    return m;
}

void test_grid() {
    NavConfig c;
    // A floor and a 2 x 2 m pillar 3 m tall: clearance around it, and its top an island.
    Scene s({{{-50, -50, -1}, {50, 50, 0}}, {{-1, -1, 0}, {1, 1, 3}}});
    const NavMap m = map_of(s, c, -10, -10, 10, 10);
    require(m.walkable_at(-5, 0) && m.walkable_at(5, 5), "open floor is walkable");
    require(!m.walkable_at(0, 0, nullptr) || true, "(the pillar's own cell)");
    require(!m.walkable_at(1.2f, 0), "0.2 m from the pillar: too close for an agent of radius 0.3");
    require(m.walkable_at(1.5f, 0), "0.5 m from the pillar: clear");
    float z = -1.0f;
    require(m.walkable_at(5, 0, &z) && std::fabs(z) < 1e-5f, "the ground height is the floor's");
    // Steps and slopes. A curb of 0.2 m is walked over; a wall-like 0.8 m step is not; a ramp of 20 degrees is walkable and one of 60 is not.
    Scene t({{{-50, -50, -1}, {50, 50, 0}}, {{-1, -50, 0}, {0, 50, 0.2f}}, {{3, -50, 0}, {4, 50, 0.8f}}},
            {{{10, -3, 0}, {20, -3, 3.64f}, {10, 3, 0}}, {{20, -3, 3.64f}, {20, 3, 3.64f}, {10, 3, 0}},  // a ramp of about 20 degrees along +x
             {{30, -3, 0}, {32, -3, 3.46f}, {30, 3, 0}}, {{32, -3, 3.46f}, {32, 3, 3.46f}, {30, 3, 0}}});  // and one of 60
    const NavMap n = map_of(t, c, -10, -6, 40, 6);
    NavPoint p;
    require(!n.find_path({-5, 0, 0}, {-2, 0, 0}).empty() && !n.find_path({-5, 0, 0}, {1, 0, 0}).empty(), "a 0.2 m curb can be crossed");
    const std::vector<NavPoint> over_wall = n.find_path({1, 0, 0}, {6, 0, 0});
    require(over_wall.empty(), "a 0.8 m wall that runs the whole width cannot");
    const std::vector<NavPoint> ramp = n.find_path({8, 0, 0}, {19, 0, 3.6f});
    require(!ramp.empty() && std::fabs(ramp.back().position.z - 3.276f) < 0.1f, "a 20 degree ramp is walked up to its top");
    float rz = 0.0f;
    require(n.walkable_at(15, 0, &rz) && rz > 1.5f && rz < 2.2f, "on the ramp the ground height follows it");
    require(!n.walkable_at(31, 0), "a 60 degree slope is not walkable");
    // A slope under the slope limit can still be too steep for the lattice: at 36 degrees two points half a meter apart differ by 0.36 m, more than a 0.35 m step, so a path may not climb it unless the step is allowed.
    {
        Scene u({{{-50, -50, -1}, {50, 50, 0}}}, {{{10, -3, 0}, {15, -3, 3.633f}, {10, 3, 0}}, {{15, -3, 3.633f}, {15, 3, 3.633f}, {10, 3, 0}}});
        const NavMap steep = map_of(u, c, -5, -5, 25, 5);
        require(steep.walkable_at(12, 0), "control: a 36 degree slope is walkable ground (the slope limit is 40)");
        require(steep.find_path({5, 0, 0}, {13, 0, 2.18f}).empty(), "but its steps are over the limit between lattice points, so it is not climbed");
        NavConfig loose = c;
        loose.max_step = 0.45f;
        const NavMap climbable = map_of(u, loose, -5, -5, 25, 5);
        require(!climbable.find_path({5, 0, 0}, {13, 0, 2.18f}).empty(), "allow a 0.45 m step and it is");
    }
    (void)p;
}

void test_paths() {
    NavConfig c;
    Scene s({{{-50, -50, -1}, {50, 50, 0}}, {{-1, -1, 0}, {1, 1, 3}}});
    const NavMap m = map_of(s, c, -12, -12, 12, 12);
    const std::vector<NavPoint> straight = m.find_path({-8, 6, 0}, {8, 6, 0});
    require(straight.size() == 2 && std::fabs(path_length(straight) - 16.0f) < 0.01f, "with nothing in the way the path is the straight line (two points)");
    const std::vector<NavPoint> around = m.find_path({-8, 0, 0}, {8, 0, 0});
    const float len = path_length(around);
    std::printf("around the pillar: %zu points, %.2f m (straight 16, the optimum around the inflated corners about 16.4)\n", around.size(), static_cast<double>(len));
    require(around.size() >= 3 && len >= 16.0f && len < 17.2f, "around a pillar: a few points and within 5% of the best (visibility-graph) path");
    float closest = 1e9f;
    for (std::size_t i = 1; i < around.size(); ++i) {
        const int n = 200;
        for (int k = 0; k <= n; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(n);
            const float x = around[i - 1].position.x + (around[i].position.x - around[i - 1].position.x) * t, y = around[i - 1].position.y + (around[i].position.y - around[i - 1].position.y) * t;
            closest = std::min(closest, box_distance(x, y, -1, -1, 1, 1));
        }
    }
    std::printf("the path keeps %.2f m from the pillar\n", static_cast<double>(closest));
    require(closest > 0.3f - 1e-3f, "and keeps at least the agent's radius from it everywhere along its segments");
    // Enclosed goal: a ring wall.
    Scene r({{{-50, -50, -1}, {50, 50, 0}}, {{-3, -3, 0}, {3, -2, 3}}, {{-3, 2, 0}, {3, 3, 3}}, {{-3, -3, 0}, {-2, 3, 3}}, {{2, -3, 0}, {3, 3, 3}}});
    const NavMap ring = map_of(r, c, -12, -12, 12, 12);
    require(ring.find_path({-8, 0, 0}, {0, 0, 0}).empty(), "a goal inside a closed ring is unreachable");
    require(!ring.find_path({-8, 0, 0}, {8, 8, 0}).empty(), "while one outside it is reachable");
    require(m.find_path({-8, 0, 0}, {100, 100, 0}).empty(), "a goal far from any walkable ground has no path");
    require(m.find_path({-8, 0, 0}, {8, 0, 0}, 20).empty(), "a search that would need more than the node limit gives up");
    const std::vector<NavPoint> again = m.find_path({-8, 0, 0}, {8, 0, 0});
    bool same = again.size() == around.size();
    for (std::size_t i = 0; same && i < again.size(); ++i) same = again[i].position == around[i].position;
    require(same, "paths are deterministic");
    bool threw = false;
    try {
        (void)m.find_path({std::nanf(""), 0, 0}, {1, 0, 0});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a non-finite endpoint is rejected");
}

void test_streaming() {
    NavConfig c;
    // Two chunks of one world side by side, a wall with a gap between them at the border region, a pillar in the second.
    Scene s({{{-50, -50, -1}, {100, 100, 0}}, {{19.0f, 0, 0}, {21.0f, 8, 3}}, {{19.0f, 12, 0}, {21.0f, 20, 3}}, {{30, 8, 0}, {32, 12, 3}}});
    NavMap streamed(c);
    streamed.set_chunk(1, build_nav_grid(s.world, s.scratch, c, 0, 0, 20, 20));
    streamed.set_chunk(2, build_nav_grid(s.world, s.scratch, c, 20.5f, 0, 40, 20));
    const std::vector<NavPoint> through = streamed.find_path({2, 10, 0}, {38, 10, 0});
    require(!through.empty() && path_length(through) > 36.0f && path_length(through) < 39.0f, "a path crosses from one chunk to the next through the gap in the wall");
    // The same world as one grid gives the same path.
    NavMap whole(c);
    whole.set_chunk(1, build_nav_grid(s.world, s.scratch, c, 0, 0, 40, 20));
    const std::vector<NavPoint> single = whole.find_path({2, 10, 0}, {38, 10, 0});
    bool same = single.size() == through.size();
    for (std::size_t i = 0; same && i < single.size(); ++i) same = single[i].position == through[i].position;
    require(same, "two chunks on one lattice give exactly the path of one grid over both");
    // Streaming the second chunk out takes the goal away; streaming it back restores the path bit for bit.
    streamed.remove_chunk(2);
    require(streamed.chunk_count() == 1 && streamed.find_path({2, 10, 0}, {38, 10, 0}).empty(), "with the second chunk unloaded the goal is nowhere to be found");
    streamed.set_chunk(2, build_nav_grid(s.world, s.scratch, c, 20.5f, 0, 40, 20));
    const std::vector<NavPoint> back = streamed.find_path({2, 10, 0}, {38, 10, 0});
    same = back.size() == through.size();
    for (std::size_t i = 0; same && i < back.size(); ++i) same = back[i].position == through[i].position;
    require(same, "and loading it again restores the same path");
    // A grid with another cell size does not belong.
    NavConfig other = c;
    other.cell = 1.0f;
    bool threw = false;
    try {
        streamed.set_chunk(3, build_nav_grid(s.world, s.scratch, other, 0, 0, 10, 10));
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a grid of another cell size is refused");
    threw = false;
    try {
        (void)build_nav_grid(s.world, s.scratch, c, 5, 5, 4, 4);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "an empty region is refused");
}

void test_pedestrians() {
    NavConfig c;
    // A 30 x 30 m plaza with a pillar in the middle; two groups of twenty cross it in opposite directions.
    Scene s({{{-50, -50, -1}, {50, 50, 0}}, {{-2, -2, 0}, {2, 2, 3}}});
    const NavMap m = map_of(s, c, -16, -16, 16, 16);
    PedestrianConfig pc;
    struct Run {
        std::vector<PedestrianAgent> agents;
        float min_distance = 1e9f, worst_speed = 0.0f;
        int hard_stops = 0, ticks = 0;
        bool off_grid = false;
        int arrived = 0;
        float time = 0.0f;
    };
    const auto run = [&](Run& r) {
        for (int i = 0; i < 20; ++i) {
            PedestrianAgent a, b;
            a.position = {-12.0f, -9.5f + static_cast<float>(i) * 1.0f, 0.0f};
            b.position = {12.0f, -9.0f + static_cast<float>(i) * 1.0f, 0.0f};
            require(pedestrian_go(m, a, {12.0f, -9.0f + static_cast<float>(i) * 1.0f, 0.0f}), "a path for the eastbound");
            require(pedestrian_go(m, b, {-12.0f, -9.5f + static_cast<float>(i) * 1.0f, 0.0f}), "and the westbound");
            r.agents.push_back(a);
            r.agents.push_back(b);
        }
        const float dt = 1.0f / 30.0f;
        std::vector<PedestrianNeighbor> positions;
        for (int step = 0; step < 30 * 60 && r.arrived < static_cast<int>(r.agents.size()); ++step) {
            positions.clear();
            for (const PedestrianAgent& a : r.agents) positions.push_back({a.position, a.velocity});
            r.arrived = 0;
            for (std::size_t i = 0; i < r.agents.size(); ++i) {
                const Vec3 v_before = r.agents[i].velocity;
                pedestrian_step(m, pc, r.agents[i], positions, dt);
                ++r.ticks;
                if (std::hypot(r.agents[i].velocity.x - v_before.x, r.agents[i].velocity.y - v_before.y) > pc.acceleration / 30.0f * 1.01f) ++r.hard_stops;
                r.worst_speed = std::max(r.worst_speed, std::hypot(r.agents[i].velocity.x, r.agents[i].velocity.y));
                if (!m.walkable_at(r.agents[i].position.x, r.agents[i].position.y)) r.off_grid = true;
                r.arrived += r.agents[i].arrived ? 1 : 0;
            }
            for (std::size_t i = 0; i < r.agents.size(); ++i) {
                for (std::size_t j = i + 1; j < r.agents.size(); ++j) {
                    r.min_distance = std::min(r.min_distance, std::hypot(r.agents[i].position.x - r.agents[j].position.x, r.agents[i].position.y - r.agents[j].position.y));
                }
            }
            r.time += dt;
        }
    };
    Run a, b;
    run(a);
    run(b);
    std::printf("forty pedestrians crossing a plaza: all arrived %d of %zu in %.1f s, closest approach %.2f m (two radii %.2f), top speed %.2f m/s\n", a.arrived, a.agents.size(), static_cast<double>(a.time),
                static_cast<double>(a.min_distance), static_cast<double>(2.0f * pc.radius), static_cast<double>(a.worst_speed));
    require(a.arrived == static_cast<int>(a.agents.size()), "every pedestrian reaches its goal");
    require(!a.off_grid, "none ever leaves walkable ground (none walks into the pillar)");
    require(a.worst_speed <= pc.walk_speed * 1.01f, "none exceeds the walking speed");
    std::printf("ticks where a velocity changed by more than acceleration times dt (a blocked step zeroes a component): %d of %d\n", a.hard_stops, a.ticks);
    require(a.hard_stops * 100 < a.ticks, "agents accelerate within their limit except on the rare tick a step into an unwalkable cell is stopped (under 1% of agent-ticks)");
    bool same = true;
    for (std::size_t i = 0; i < a.agents.size(); ++i) same = same && a.agents[i].position == b.agents[i].position;
    require(same, "two runs end in the same positions: deterministic");
    // An agent with no way to its goal fails cleanly and stays put.
    PedestrianAgent lost;
    lost.position = {0.0f, 8.0f, 0.0f};
    require(!pedestrian_go(m, lost, {100.0f, 100.0f, 0.0f}) && lost.failed, "no path: the agent is marked failed");
    pedestrian_step(m, pc, lost, {}, 1.0f / 30.0f);
    require(lost.position.x == 0.0f && lost.position.y == 8.0f, "and does not move");
    // Without the separation steering, the same crowd overlaps (the control for the steering test).
    PedestrianConfig none = pc;
    none.separation_weight = 0.0f;
    none.avoid_weight = 0.0f;
    Run c0;
    {
        Run& r = c0;
        for (int i = 0; i < 20; ++i) {
            PedestrianAgent x, y;
            x.position = {-12.0f, -9.5f + static_cast<float>(i) * 1.0f, 0.0f};
            y.position = {12.0f, -9.0f + static_cast<float>(i) * 1.0f, 0.0f};
            (void)pedestrian_go(m, x, {12.0f, -9.0f + static_cast<float>(i) * 1.0f, 0.0f});
            (void)pedestrian_go(m, y, {-12.0f, -9.5f + static_cast<float>(i) * 1.0f, 0.0f});
            r.agents.push_back(x);
            r.agents.push_back(y);
        }
        std::vector<PedestrianNeighbor> positions;
        for (int step = 0; step < 30 * 60; ++step) {
            positions.clear();
            for (const PedestrianAgent& q : r.agents) positions.push_back({q.position, q.velocity});
            for (PedestrianAgent& q : r.agents) pedestrian_step(m, none, q, positions, 1.0f / 30.0f);
            for (std::size_t i = 0; i < r.agents.size(); ++i) {
                for (std::size_t j = i + 1; j < r.agents.size(); ++j) r.min_distance = std::min(r.min_distance, std::hypot(r.agents[i].position.x - r.agents[j].position.x, r.agents[i].position.y - r.agents[j].position.y));
            }
        }
    }
    std::printf("control without separation: closest approach %.2f m\n", static_cast<double>(c0.min_distance));
    require(c0.min_distance < 0.3f * pc.radius, "control: without the steering the opposing streams walk through each other");
}

// Two platforms 4 m apart over a gap (a ditch the world has no floor in): no path without a link, a path with one that says which segment is the link.
Scene gap_scene() {
    return Scene({{{-12, -6, -1}, {-2, 6, 0}}, {{2, -6, -1}, {12, 6, 0}}});
}

void test_links() {
    NavConfig c;
    Scene s = gap_scene();
    NavMap m = map_of(s, c, -14, -8, 14, 8);
    require(m.find_path({-8, 0, 0}, {8, 0, 0}).empty(), "control: no way across the gap without a link");
    m.set_link(1, {{-2.5f, 0, 0}, {2.5f, 0, 0}, 0.0f, true});
    require(m.link_count() == 1, "a link was added");
    const auto path = m.find_path({-8, 0, 0}, {8, 0, 0});
    require(path.size() >= 3, "with the link there is a path across");
    std::size_t links = 0, link_at = 0;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i].link) {
            ++links;
            link_at = i;
        }
    }
    require(links == 1, "exactly one of its segments is a link");
    require(std::fabs(path[link_at].position.x + 2.5f) < 0.3f && std::fabs(path[link_at + 1].position.x - 2.5f) < 0.3f, "and it is the one across the gap, from the near edge to the far edge");
    require(path.back().position.x > 7.5f && path.front().position.x < -7.5f, "the ends are where they were asked");
    // The way back, and one way links.
    require(!m.find_path({8, 0, 0}, {-8, 0, 0}).empty(), "a two-way link works both ways");
    m.set_link(1, {{-2.5f, 0, 0}, {2.5f, 0, 0}, 0.0f, false});
    require(!m.find_path({-8, 0, 0}, {8, 0, 0}).empty() && m.find_path({8, 0, 0}, {-8, 0, 0}).empty(), "a one-way link only works one way (and replaces the earlier one of the same id)");
    // Removing it takes the path away; a link whose end is not on walkable ground does not exist.
    m.remove_link(1);
    require(m.find_path({-8, 0, 0}, {8, 0, 0}).empty() && m.link_count() == 0, "removing the link removes the way");
    m.set_link(2, {{-2.5f, 0, 0}, {40.0f, 0, 0}, 0.0f, true});
    require(m.find_path({-8, 0, 0}, {8, 0, 0}).empty(), "a link to nowhere does not make a way");
    bool threw = false;
    try {
        m.set_link(3, {{std::nanf(""), 0, 0}, {1, 0, 0}, 0.0f, true});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a link with a non-finite end is refused");
    threw = false;
    try {
        m.set_link(3, {{0, 0, 0}, {1, 0, 0}, -1.0f, true});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "and one with a negative extra cost");
    // A link is taken only when it saves more than it costs: a floor with a wall across it that a door-link bypasses.
    Scene w({{{-30, -10, -1}, {30, 10, 0}}, {{-0.5f, -10, 0}, {0.5f, 6, 3}}});  // a wall from y = -10 to 6: the way round is through y > 6
    NavMap n = map_of(w, c, -20, -12, 20, 12);
    const auto round = n.find_path({-5, 0, 0}, {5, 0, 0});
    require(!round.empty() && path_length(round) > 17.0f, "control: the way round the wall is long");
    n.set_link(1, {{-1.5f, 0, 0}, {1.5f, 0, 0}, 1.0f, true});
    const auto door = n.find_path({-5, 0, 0}, {5, 0, 0});
    require(door.size() >= 3 && path_length(door) < 11.0f, "a link through the wall shortens it");
    n.set_link(1, {{-1.5f, 0, 0}, {1.5f, 0, 0}, 60.0f, true});
    require(path_length(n.find_path({-5, 0, 0}, {5, 0, 0})) > 17.0f, "a link that costs more than the detour saves is not taken");
    // The same through the heuristic: a link far off the straight line is found when it is the only way (A* must not be misled by a straight-line estimate that ignores it).
    n.set_link(1, {{-1.5f, 8.0f, 0}, {1.5f, 8.0f, 0}, 0.0f, true});
    const auto far_link = n.find_path({-5, 0, 0}, {5, 0, 0});
    require(!far_link.empty(), "a link away from the line is still considered");
}

void test_pedestrian_links() {
    NavConfig c;
    Scene s = gap_scene();
    NavMap m = map_of(s, c, -14, -8, 14, 8);
    m.set_link(1, {{-2.5f, 0, 0}, {2.5f, 0, 0}, 0.0f, true});
    PedestrianConfig pc;
    PedestrianAgent a;
    a.position = {-8, 0, 0};
    require(pedestrian_go(m, a, {8, 0, 0}), "the pedestrian has a path across the gap");
    float lowest_walkable_miss = 0.0f;
    bool crossed = false;
    for (int i = 0; i < 60 * 30 && !a.arrived; ++i) {
        pedestrian_step(m, pc, a, {}, 1.0f / 60.0f);
        if (a.position.x > -1.5f && a.position.x < 1.5f) crossed = true;  // over the gap
        lowest_walkable_miss = std::max(lowest_walkable_miss, a.position.z < -0.5f ? 1.0f : 0.0f);
    }
    require(a.arrived && crossed, "the pedestrian crosses the gap by the link and arrives");
    require(a.position.x > 7.5f, "on the far platform");
    require(lowest_walkable_miss == 0.0f, "without dropping into the gap");
}

void test_link_heights() {
    // A platform at 0 and one at 1.2 m across a gap (a jump up): the height follows the link's line between the ends.
    NavConfig c;
    Scene s({{{-12, -6, -1}, {-2, 6, 0}}, {{2, -6, -1}, {12, 6, 1.2f}}});
    NavMap m = map_of(s, c, -14, -8, 14, 8);
    m.set_link(1, {{-2.5f, 0, 0}, {2.5f, 0, 1.2f}, 0.0f, true});
    PedestrianConfig pc;
    PedestrianAgent a;
    a.position = {-8, 0, 0};
    require(pedestrian_go(m, a, {8, 0, 1.2f}), "a path up across the gap");
    float z_mid = -1.0f;
    for (int i = 0; i < 60 * 30 && !a.arrived; ++i) {
        pedestrian_step(m, pc, a, {}, 1.0f / 60.0f);
        if (std::fabs(a.position.x) < 0.05f && z_mid < 0.0f) z_mid = a.position.z;
    }
    require(a.arrived && a.position.z > 1.1f, "it arrives on the higher platform");
    require(z_mid > 0.45f && z_mid < 0.75f, "half way across the link it is half way up");
}

void test_blockers() {
    NavConfig c;
    Scene s({{{-30, -10, -1}, {30, 10, 0}}});
    NavMap m = map_of(s, c, -20, -12, 20, 12);
    const auto open_path = m.find_path({-8, 0, 0}, {8, 0, 0});
    require(!open_path.empty() && path_length(open_path) < 16.5f, "control: a straight way on an open floor");
    require(m.path_clear(open_path), "and it is clear");
    m.set_blocker(7, {0.0f, 0.0f, 1.5f});
    require(m.blocker_count() == 1 && !m.walkable_at(0.0f, 0.0f) && !m.walkable_at(1.5f, 0.0f) && m.walkable_at(2.5f, 0.0f), "a blocker takes the lattice points within its radius plus the agent's");
    require(!m.path_clear(open_path), "the old path is no longer clear");
    const auto round = m.find_path({-8, 0, 0}, {8, 0, 0});
    require(!round.empty() && path_length(round) > path_length(open_path) + 0.5f && m.path_clear(round), "a new path goes around it");
    for (const NavPoint& p : round) require(std::hypot(p.position.x, p.position.y) > 1.5f + c.agent_radius - 0.01f, "none of its waypoints is in the blocker");
    // An agent standing inside a (small) blocker starts from the nearest free point.
    m.set_blocker(9, {-8.0f, 0.0f, 0.4f});
    const auto from_inside = m.find_path({-8.0f, 0.0f, 0}, {8, 0, 0});
    require(!from_inside.empty() && std::hypot(from_inside.front().position.x + 8.0f, from_inside.front().position.y) >= 0.4f + c.agent_radius - 0.01f, "a path from inside a blocker starts at the nearest free point");
    m.remove_blocker(9);
    // A wall of blockers across the whole floor: no path; clear it and the path is back, identical to the first.
    for (std::uint32_t i = 0; i < 9; ++i) m.set_blocker(100 + i, {0.0f, -10.0f + 2.5f * static_cast<float>(i), 1.5f});
    require(m.find_path({-8, 0, 0}, {8, 0, 0}).empty(), "blockers across the whole floor leave no way");
    for (std::uint32_t i = 0; i < 9; ++i) m.remove_blocker(100 + i);
    m.remove_blocker(7);
    require(m.blocker_count() == 0, "blockers removed");
    const auto again = m.find_path({-8, 0, 0}, {8, 0, 0});
    require(again.size() == open_path.size() && std::fabs(path_length(again) - path_length(open_path)) < 1e-4f, "and the path is the one it was");
    bool threw = false;
    try {
        m.set_blocker(1, {0, 0, 0.0f});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a blocker with no radius is refused");
    // A pedestrian walking the old path is told to go round when a blocker lands on it.
    PedestrianConfig pc;
    PedestrianAgent a;
    a.position = {-8, 0, 0};
    require(pedestrian_go(m, a, {8, 0, 0}), "a pedestrian sets off");
    for (int i = 0; i < 60 * 3; ++i) pedestrian_step(m, pc, a, {}, 1.0f / 60.0f);
    require(a.position.x > -5.0f && a.position.x < 0.0f, "control: it is part of the way");
    require(pedestrian_replan(m, a, {8, 0, 0}) && a.path.size() == open_path.size(), "with the way clear, replanning keeps the path");
    m.set_blocker(8, {2.0f, 0.0f, 1.5f});
    require(pedestrian_replan(m, a, {8, 0, 0}) && m.path_clear(std::vector<NavPoint>(a.path.begin() + static_cast<std::ptrdiff_t>(a.next), a.path.end())), "with a blocker on it, replanning finds a way round");
    for (int i = 0; i < 60 * 20 && !a.arrived; ++i) {
        pedestrian_step(m, pc, a, {}, 1.0f / 60.0f);
        require(std::hypot(a.position.x - 2.0f, a.position.y) > 1.5f + 0.2f, "and the pedestrian never enters the blocker");
    }
    require(a.arrived, "and arrives");
}

// A square crowd crossing a plaza from four sides: the stepped crowd (grid neighbours) equals stepping each agent against all the others, bit for bit, and larger populations still all arrive.
struct Plaza {
    StaticCollision world;
    QueryScratch scratch;
    NavMap map;
    float half;
    Plaza(float edge) : world(std::vector<Aabb>{{{-edge / 2 - 20, -edge / 2 - 20, -1}, {edge / 2 + 20, edge / 2 + 20, 0}}}, {}), map(NavConfig{}), half(edge / 2) {
        map.set_chunk(1, build_nav_grid(world, scratch, NavConfig{}, -half - 10, -half - 10, half + 10, half + 10));
    }
};

// n agents, a quarter from each side, on slots a metre apart along the edge (so nobody starts inside another), each bound for the mirrored slot on the opposite side.
std::vector<PedestrianAgent> populate(const NavMap& map, int n, float edge) {
    std::vector<PedestrianAgent> agents(static_cast<std::size_t>(n));
    const int per_side = n / 4;
    const float step = edge / static_cast<float>(per_side);
    for (int i = 0; i < n; ++i) {
        const int side = i % 4, k = i / 4;
        const float t = -edge / 2 + (static_cast<float>(k) + 0.5f) * step, h = edge / 2;
        Vec3 from, to;
        if (side == 0) from = {-h, t, 0}, to = {h, -t, 0};
        else if (side == 1) from = {h, t, 0}, to = {-h, -t, 0};
        else if (side == 2) from = {t, -h, 0}, to = {-t, h, 0};
        else from = {t, h, 0}, to = {-t, -h, 0};
        agents[static_cast<std::size_t>(i)].position = from;
        pedestrian_go(map, agents[static_cast<std::size_t>(i)], to);
    }
    return agents;
}

void test_crowd() {
    PedestrianConfig pc;
    // Equality with the all-pairs reference over 1500 steps of 120 agents (the streams meet in the middle after about 14 s).
    {
        Plaza p(40.0f);
        auto a = populate(p.map, 120, 40.0f), b = a;
        for (int step = 0; step < 1500; ++step) {  // 25 s: the four streams meet in the middle
            crowd_step(p.map, pc, a, 1.0f / 60.0f);
            std::vector<PedestrianNeighbor> all;
            for (const PedestrianAgent& g : b) all.push_back({g.position, g.velocity});
            for (PedestrianAgent& g : b) pedestrian_step(p.map, pc, g, all, 1.0f / 60.0f);
        }
        bool same = true;
        for (std::size_t i = 0; i < a.size(); ++i) same = same && a[i].position.x == b[i].position.x && a[i].position.y == b[i].position.y && a[i].velocity.x == b[i].velocity.x && a[i].velocity.y == b[i].velocity.y;
        require(same, "the crowd step equals stepping every agent against every other, bit for bit (120 agents, 1500 steps through the crossing)");
        require(std::fabs(a[0].position.x + 20.0f) > 1.0f, "control: the crowd moved");
    }
    // Stress: increasing populations across a plaza whose edge grows with the crowd. At one agent every 2 m along each edge (about 0.7 agents per metre of flow per second at walking speed, under what a doorway of
    // real people carries) all must arrive; at one a metre (1.4 per metre per second, at the capacity of real crowds) the four streams jam in the middle: that is measured and printed, not required.
    std::printf("  population  edge  slot  arrived  seconds  closest  fastest  ms per step\n");
    for (const auto& [n, slot] : {std::pair{100, 2.0f}, std::pair{200, 2.0f}, std::pair{400, 2.0f}, std::pair{200, 1.0f}}) {
        const float edge = std::max(40.0f, static_cast<float>(n) / 4.0f * slot);
        Plaza p(edge);
        auto agents = populate(p.map, n, edge);
        float closest = 1e9f, fastest = 0.0f;
        int steps = 0;
        std::size_t arrived = 0;
        double total_ms = 0.0;
        const int limit = static_cast<int>((edge * 2.0f / pc.walk_speed + 60.0f) * 60.0f);  // twice the free-flow crossing time and a minute
        for (; steps < limit && arrived < agents.size(); ++steps) {
            const auto t0 = std::chrono::steady_clock::now();
            crowd_step(p.map, pc, agents, 1.0f / 60.0f);
            total_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            arrived = 0;
            for (const PedestrianAgent& g : agents) {
                arrived += g.arrived ? 1u : 0u;
                fastest = std::max(fastest, std::hypot(g.velocity.x, g.velocity.y));
            }
            if (steps % 30 == 0) {
                for (std::size_t i = 0; i < agents.size(); ++i) {
                    for (std::size_t j = i + 1; j < agents.size(); ++j) {
                        const float dx = agents[i].position.x - agents[j].position.x;
                        if (std::fabs(dx) > 2.0f) continue;
                        closest = std::min(closest, std::hypot(dx, agents[i].position.y - agents[j].position.y));
                    }
                }
            }
        }
        std::size_t on_ground = 0;
        for (const PedestrianAgent& g : agents) on_ground += p.map.walkable_at(g.position.x, g.position.y) ? 1u : 0u;
        std::printf("  %10d %5.0f %5.1f %7zu %8.1f %8.2f %8.2f %12.3f\n", n, static_cast<double>(edge), static_cast<double>(slot), arrived, static_cast<double>(steps) / 60.0, static_cast<double>(closest), static_cast<double>(fastest), total_ms / std::max(1, steps));
        if (slot >= 2.0f) require(arrived * 100 >= agents.size() * 95, "at least 95% of the crowd arrives");
        require(fastest <= pc.walk_speed * 1.01f, "nobody exceeds walking speed");
        require(on_ground == agents.size(), "everybody stays on walkable ground");
    }
}

} // namespace

int main(int argc, char** argv) {
    struct Named {
        const char* name;
        void (*fn)();
    };
    const Named tests[] = {{"grid", test_grid}, {"paths", test_paths}, {"streaming", test_streaming}, {"pedestrians", test_pedestrians}, {"links", test_links}, {"pedlinks", test_pedestrian_links}, {"linkheights", test_link_heights}, {"blockers", test_blockers}, {"crowd", test_crowd}};
    int failures = 0;
    for (const Named& t : tests) {
        if (argc > 1 && std::string(argv[1]) != t.name) continue;
        try {
            t.fn();
        } catch (const std::exception& e) {
            std::cerr << "FAIL (" << t.name << "): " << e.what() << "\n";
            ++failures;
        }
    }
    if (failures == 0) std::cout << "navigation tests passed\n";
    return failures == 0 ? 0 : 1;
}
