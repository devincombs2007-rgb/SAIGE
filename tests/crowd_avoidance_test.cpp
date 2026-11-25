#include "saige/crowd_avoidance.hpp"

#include <cstdlib>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>

#define SAIGE_CHECK(cond)                                                     \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::cerr << "SAIGE_CHECK FAILED at " << __FILE__ << ":" << __LINE__    \
                << " (" << #cond << ")\n";                                    \
      std::exit(1);                                                           \
    }                                                                         \
  } while (false)

using namespace saige::crowd;

namespace {

// Independent O(N^2) brute-force overlap validator (does not rely on solver spatial hash)
struct IndependentOverlapReport {
  double min_separation_m = 0.0;
  double max_overlap_depth_m = 0.0;
  std::uint32_t overlap_pairs = 0;
};

IndependentOverlapReport measure_bruteforce_overlap(
    const std::vector<CrowdAgent>& agents) {
  IndependentOverlapReport rep{};
  rep.min_separation_m = std::numeric_limits<double>::infinity();
  for (std::size_t i = 0; i < agents.size(); ++i) {
    for (std::size_t j = i + 1; j < agents.size(); ++j) {
      const double dx = agents[j].position.x - agents[i].position.x;
      const double dy = agents[j].position.y - agents[i].position.y;
      const double dist = std::sqrt(dx * dx + dy * dy);
      const double sep = dist - (agents[i].body_radius + agents[j].body_radius);
      rep.min_separation_m = std::min(rep.min_separation_m, sep);
      if (sep < 0.0) {
        ++rep.overlap_pairs;
        rep.max_overlap_depth_m = std::max(rep.max_overlap_depth_m, -sep);
      }
    }
  }
  if (!std::isfinite(rep.min_separation_m)) {
    rep.min_separation_m = 0.0;
  }
  return rep;
}

// Regression test for Build 001 Defect 2: step(0) before grid init
void test_step_zero_before_init_no_crash() {
  CrowdSolver solver;
  CrowdAgent a{};
  a.id = 1;
  a.position = {0.0, 0.0};
  a.destination = {10.0, 0.0};
  CrowdAgent b{};
  b.id = 2;
  b.position = {0.5, 0.0};
  b.destination = {-10.0, 0.0};
  SAIGE_CHECK(solver.add_agent(a));
  SAIGE_CHECK(solver.add_agent(b));

  const CrowdStepMetrics m0 = solver.step(0.0);
  SAIGE_CHECK(m0.total_agents == 2);
  SAIGE_CHECK(m0.overlap_pair_count == 1);
  const CrowdStepMetrics m_neg = solver.step(-0.5);
  SAIGE_CHECK(m_neg.total_agents == 2);
}

void test_dense_four_way_crossing() {
  CrowdSolver solver;
  constexpr std::uint32_t kPerSide = 50;  // 200 total agents
  AgentId id = 1;
  for (std::uint32_t i = 0; i < kPerSide; ++i) {
    const double offset = -20.0 + static_cast<double>(i) * 0.8;
    CrowdAgent we{};
    we.id = id++;
    we.position = {-24.0, offset};
    we.destination = {24.0, offset};
    SAIGE_CHECK(solver.add_agent(we));

    CrowdAgent ew{};
    ew.id = id++;
    ew.position = {24.0, offset + 0.35};
    ew.destination = {-24.0, offset + 0.35};
    SAIGE_CHECK(solver.add_agent(ew));

    CrowdAgent sn{};
    sn.id = id++;
    sn.position = {offset, -24.0};
    sn.destination = {offset, 24.0};
    SAIGE_CHECK(solver.add_agent(sn));

    CrowdAgent ns{};
    ns.id = id++;
    ns.position = {offset + 0.35, 24.0};
    ns.destination = {offset + 0.35, -24.0};
    SAIGE_CHECK(solver.add_agent(ns));
  }

  double worst_overlap_all_ticks = 0.0;
  double min_sep_all_ticks = std::numeric_limits<double>::infinity();
  CrowdStepMetrics last{};
  const double dt = 1.0 / 30.0;
  for (int step = 0; step < 1200; ++step) {
    last = solver.step(dt);
    const auto rep = measure_bruteforce_overlap(solver.agents());
    worst_overlap_all_ticks = std::max(worst_overlap_all_ticks, rep.max_overlap_depth_m);
    min_sep_all_ticks = std::min(min_sep_all_ticks, rep.min_separation_m);
  }

  std::cout << "[dense-4way-200] arrived=" << last.arrived_agents << "/"
            << last.total_agents << " min_sep_all_ticks=" << min_sep_all_ticks
            << "m max_overlap_all_ticks=" << worst_overlap_all_ticks
            << "m deadlocks=" << last.deadlock_count
            << " osc=" << last.oscillation_events << "\n" << std::flush;

  SAIGE_CHECK(worst_overlap_all_ticks < 0.02);
  SAIGE_CHECK(last.arrived_agents >= 180);
}

void test_narrow_entrance_and_opposing_flows() {
  CrowdSolver solver;
  solver.set_wall_segments({
      {1, {0.0, 1.5}, {0.0, 20.0}},
      {2, {0.0, -1.5}, {0.0, -20.0}},
  });
  solver.set_doorways({{100, {0.0, 0.0}, {1.0, 0.0}, 1.5, 5.5}});

  AgentId id = 1;
  struct GoalPair { AgentId id; Vec2d final_dest; bool from_left; bool switched; };
  std::vector<GoalPair> goals;
  for (int r = 0; r < 10; ++r) {
    for (int c = 0; c < 3; ++c) {
      CrowdAgent left{};
      left.id = id++;
      left.position = {-6.0 - static_cast<double>(c) * 1.1,
                       -4.5 + static_cast<double>(r) * 1.0};
      left.personal_space = 0.06;
      left.priority_weight = 1.0 + 0.05 * static_cast<double>(id % 3);
      left.destination = {0.8, -0.75};
      SAIGE_CHECK(solver.add_agent(left));
      goals.push_back({left.id, {8.0 + static_cast<double>(c) * 1.1, -4.5 + static_cast<double>(r) * 1.0}, true, false});

      CrowdAgent right{};
      right.id = id++;
      right.position = {6.0 + static_cast<double>(c) * 1.1,
                        -4.5 + static_cast<double>(r) * 1.0};
      right.personal_space = 0.06;
      right.priority_weight = 1.0 + 0.05 * static_cast<double>(id % 3);
      right.destination = {-0.8, 0.75};
      SAIGE_CHECK(solver.add_agent(right));
      goals.push_back({right.id, {-8.0 - static_cast<double>(c) * 1.1, -4.5 + static_cast<double>(r) * 1.0}, false, false});
    }
  }

  double worst_overlap_all_ticks = 0.0;
  CrowdStepMetrics last{};
  for (int step = 0; step < 2400; ++step) {
    for (auto& g : goals) {
      if (!g.switched) {
        const CrowdAgent* a = solver.find_agent(g.id);
        if (a != nullptr && ((g.from_left && a->position.x > 0.4) || (!g.from_left && a->position.x < -0.4))) {
          solver.set_agent_destination(g.id, g.final_dest);
          g.switched = true;
        }
      }
    }
    last = solver.step(1.0 / 30.0);
    if (step % 10 == 0) {
      const auto rep = measure_bruteforce_overlap(solver.agents());
      worst_overlap_all_ticks = std::max(worst_overlap_all_ticks, rep.max_overlap_depth_m);
    }
  }

  std::cout << "[narrow-door-60] arrived=" << last.arrived_agents << "/"
            << last.total_agents << " max_overlap=" << worst_overlap_all_ticks
            << "m deadlocks=" << last.deadlock_count << "\n" << std::flush;
  SAIGE_CHECK(worst_overlap_all_ticks < 0.02);
  SAIGE_CHECK(last.arrived_agents >= 45);
}

void test_moving_obstacle_and_dynamic_destination() {
  CrowdSolver solver;
  CrowdAgent ped{};
  ped.id = 1;
  ped.position = {0.0, 0.0};
  ped.destination = {20.0, 0.0};
  SAIGE_CHECK(solver.add_agent(ped));

  for (int step = 0; step < 600; ++step) {
    const double t = static_cast<double>(step) / 30.0;
    MovingObstacle cart{10, {8.0, -6.0 + t * 2.5}, {0.0, 2.5}, 1.0};
    solver.set_moving_obstacles({cart});
    if (step == 120) {
      SAIGE_CHECK(solver.set_agent_destination(1, {20.0, 6.0}));
    }
    const auto m = solver.step(1.0 / 30.0);
    SAIGE_CHECK(m.max_overlap_depth_m < 0.01);
  }
  SAIGE_CHECK(solver.find_agent(1)->arrived);
}

void test_crowd_scaling_benchmarks() {
  const std::uint32_t counts[] = {100, 500, 1000, 2000};
  for (std::uint32_t n : counts) {
    CrowdSolver solver;
    const double side = std::sqrt(static_cast<double>(n)) * 3.0;
    const std::uint32_t cols = static_cast<std::uint32_t>(std::sqrt(static_cast<double>(n)));
    for (std::uint32_t i = 0; i < n; ++i) {
      CrowdAgent a{};
      a.id = i + 1;
      const double x = static_cast<double>(i % cols) * 2.2 - side * 0.35;
      const double y = static_cast<double>(i / cols) * 2.2 - side * 0.35;
      a.position = {x, y};
      a.destination = {-x, -y};
      SAIGE_CHECK(solver.add_agent(a));
    }

    const auto t0 = std::chrono::steady_clock::now();
    CrowdStepMetrics last{};
    for (int step = 0; step < 60; ++step) {
      last = solver.step(1.0 / 30.0);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double avg_ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count() / 60.0;

    const auto rep = measure_bruteforce_overlap(solver.agents());
    std::cout << "[crowd-scale n=" << n << "] avg_ms=" << avg_ms
              << " min_sep=" << rep.min_separation_m
              << "m overlap_pairs=" << rep.overlap_pairs
              << " max_overlap=" << rep.max_overlap_depth_m
              << "m mem_kb=" << (last.estimated_memory_bytes / 1024) << "\n" << std::flush;
    SAIGE_CHECK(rep.max_overlap_depth_m < 0.02);
  }
}

}  // namespace

int main() {
  test_step_zero_before_init_no_crash();
  test_dense_four_way_crossing();
  test_narrow_entrance_and_opposing_flows();
  test_moving_obstacle_and_dynamic_destination();
  test_crowd_scaling_benchmarks();
  std::cout << "crowd_avoidance_test: ALL 5 SUITES PASSED\n";
  return 0;
}
