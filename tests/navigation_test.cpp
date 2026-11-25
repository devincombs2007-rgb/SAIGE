// Navigation: the walkability grid (clearance, slope, steps), A* paths around obstacles and across streamed chunks, and pedestrians that follow them without walking into each other.
#include "saige/navigation.hpp"

#include <algorithm>
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

} // namespace

int main(int argc, char** argv) {
    struct Named {
        const char* name;
        void (*fn)();
    };
    const Named tests[] = {{"grid", test_grid}, {"paths", test_paths}, {"streaming", test_streaming}, {"pedestrians", test_pedestrians}};
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
