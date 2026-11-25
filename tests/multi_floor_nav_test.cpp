#include "saige/multi_floor_nav.hpp"

#include <cmath>
#include <cstdlib>
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

using namespace saige::multi_floor_nav;

namespace {

void test_two_story_building_and_stairs() {
  MultiFloorNavGraph nav(1.0, 0.35);

  for (int x = 0; x <= 5; ++x) {
    SAIGE_CHECK(nav.set_slice({{x, 0, 0}, 0.0, true, x >= 3}));
  }
  for (int x = 3; x <= 5; ++x) {
    SAIGE_CHECK(nav.set_slice({{x, 0, 1}, 3.5, true, true}));
  }

  const auto blocked = nav.find_path({0, 0, 0}, {5, 0, 1});
  SAIGE_CHECK(!blocked.found);

  SAIGE_CHECK(nav.add_portal({1, {4, 0, 0}, {4, 0, 1}, PortalKind::Stair, 4.0, 2.5, true}));

  const auto path = nav.find_path({0, 0, 0}, {5, 0, 1});
  SAIGE_CHECK(path.found);
  SAIGE_CHECK(path.portal_transitions == 1);
  SAIGE_CHECK(path.interior_steps >= 3);
  SAIGE_CHECK(path.waypoints.front() == (NavNodeId{0, 0, 0}));
  SAIGE_CHECK(path.waypoints.back() == (NavNodeId{5, 0, 1}));
}

void test_ramp_step_height_limit() {
  MultiFloorNavGraph nav(1.0, 0.35);
  SAIGE_CHECK(nav.set_slice({{0, 0, 0}, 0.0, true, false}));
  SAIGE_CHECK(nav.set_slice({{1, 0, 0}, 0.30, true, false}));
  SAIGE_CHECK(nav.set_slice({{2, 0, 0}, 0.60, true, false}));
  SAIGE_CHECK(nav.set_slice({{3, 0, 0}, 1.20, true, false}));

  SAIGE_CHECK(nav.find_path({0, 0, 0}, {2, 0, 0}).found);
  SAIGE_CHECK(!nav.find_path({0, 0, 0}, {3, 0, 0}).found);
}

// Regression 1: Diagonal movement must not cut corners across blocked/absent cells
void test_no_diagonal_corner_cutting() {
  MultiFloorNavGraph nav(1.0, 0.35);
  SAIGE_CHECK(nav.set_slice({{0, 0, 0}, 0.0, true, false}));
  SAIGE_CHECK(nav.set_slice({{1, 1, 0}, 0.0, true, false}));
  // Block (1,0) and leave (0,1) absent -> diagonal (0,0)->(1,1) must be forbidden
  SAIGE_CHECK(nav.set_slice({{1, 0, 0}, 0.0, false, false}));
  SAIGE_CHECK(!nav.find_path({0, 0, 0}, {1, 1, 0}).found);

  // Unblocking (1,0) still leaves (0,1) absent -> diagonal still forbidden,
  // must route orthogonally (0,0)->(1,0)->(1,1)
  SAIGE_CHECK(nav.set_slice({{1, 0, 0}, 0.0, true, false}));
  const auto path = nav.find_path({0, 0, 0}, {1, 1, 0});
  SAIGE_CHECK(path.found);
  SAIGE_CHECK(path.waypoints.size() == 3);
}

// Regression 2: Low-cost portal optimality (0.2 vs 2.0)
void test_low_cost_portal_optimal_route() {
  MultiFloorNavGraph nav(1.0, 0.35);
  SAIGE_CHECK(nav.set_slice({{0, 0, 0}, 0.0, true, false}));
  SAIGE_CHECK(nav.set_slice({{1, 0, 0}, 0.0, true, false}));
  SAIGE_CHECK(nav.set_slice({{2, 0, 0}, 0.0, true, false}));
  SAIGE_CHECK(nav.add_portal({1, {0, 0, 0}, {2, 0, 0}, PortalKind::Elevator, 0.2, 1.0, true}));

  const auto path = nav.find_path({0, 0, 0}, {2, 0, 0});
  SAIGE_CHECK(path.found);
  SAIGE_CHECK(std::abs(path.total_cost - 0.2) < 1e-6);
  SAIGE_CHECK(path.waypoints.size() == 2);
}

// Regression 3: INT32_MAX / INT32_MIN neighbor offset overflow safety
void test_extreme_grid_bounds_no_overflow() {
  MultiFloorNavGraph nav(1.0, 0.35);
  constexpr std::int32_t kMax = std::numeric_limits<std::int32_t>::max();
  constexpr std::int32_t kMin = std::numeric_limits<std::int32_t>::min();
  SAIGE_CHECK(nav.set_slice({{kMax, kMax, 0}, 0.0, true, false}));
  SAIGE_CHECK(nav.set_slice({{kMax - 1, kMax, 0}, 0.0, true, false}));
  SAIGE_CHECK(nav.set_slice({{kMin, kMin, 0}, 0.0, true, false}));

  const auto p = nav.find_path({kMax - 1, kMax, 0}, {kMax, kMax, 0});
  SAIGE_CHECK(p.found);
  SAIGE_CHECK(!nav.find_path({kMin, kMin, 0}, {kMax, kMax, 0}).found);
}

}  // namespace

void test_smooth_portal_traversal_state() {
  PortalTraversalProgress prog{};
  prog.begin(PortalKind::Stair, {4, 0, 0}, {4, 0, 1}, 0.0, 3.5, 2.0);
  SAIGE_CHECK(prog.is_active());
  SAIGE_CHECK(prog.state == PortalTraversalState::TraversingStair);
  SAIGE_CHECK(std::abs(prog.current_z_m() - 0.0) < 1e-6);

  prog.step(1.0);
  SAIGE_CHECK(prog.is_active());
  SAIGE_CHECK(std::abs(prog.current_z_m() - 1.75) < 1e-6);

  prog.step(1.0);
  SAIGE_CHECK(!prog.is_active());
  SAIGE_CHECK(prog.is_complete());
  SAIGE_CHECK(std::abs(prog.current_z_m() - 3.5) < 1e-6);
}

int main() {
  test_two_story_building_and_stairs();
  test_ramp_step_height_limit();
  test_no_diagonal_corner_cutting();
  test_low_cost_portal_optimal_route();
  test_extreme_grid_bounds_no_overflow();
  test_smooth_portal_traversal_state();
  std::cout << "multi_floor_nav_test: ALL 6 SUITES PASSED\n";
  return 0;
}
