#include "saige/npc_psychology.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>

#define SAIGE_CHECK(cond)                                                     \
  do {                                                                        \
    if (!(cond)) {                                                            \
      std::cerr << "SAIGE_CHECK FAILED at " << __FILE__ << ":" << __LINE__    \
                << " (" << #cond << ")\n";                                    \
      std::exit(1);                                                           \
    }                                                                         \
  } while (false)

using namespace saige::psychology;
using namespace saige::world_sim;

namespace {

void test_individual_habit_variation_is_deterministic() {
  NpcPsychologyProfile a1(101);
  NpcPsychologyProfile a2(101);
  NpcPsychologyProfile b(202);

  SAIGE_CHECK(std::abs(a1.habits().punctuality - a2.habits().punctuality) < 1e-9);
  SAIGE_CHECK(std::abs(a1.habits().punctuality - b.habits().punctuality) > 1e-3);
}

void test_experience_memory_and_environmental_reactions() {
  NpcPsychologyProfile prof(55);
  prof.record_experience({10, 0, -0.8, 10.0});
  SAIGE_CHECK(prof.venue_affinity(10) < -0.5);

  EnvironmentalCondition calm{};
  const auto avoid_dec = prof.evaluate(calm, 10);
  if (prof.habits().risk_tolerance < 0.7) {
    SAIGE_CHECK(avoid_dec.action == ContextualAction::AvoidArea);
  }

  EnvironmentalCondition storm{};
  storm.storm_severity = 0.85;
  storm.rain_intensity = 0.9;
  const auto storm_dec = prof.evaluate(storm, 20);
  SAIGE_CHECK(storm_dec.action == ContextualAction::SeekShelter);

  EnvironmentalCondition flood{};
  flood.flood_depth_m = 0.45;
  const auto flood_dec = prof.evaluate(flood, 20);
  SAIGE_CHECK(flood_dec.action == ContextualAction::FleeHazard);
}

void test_emotional_stimulus_and_decay() {
  NpcPsychologyProfile prof(7);
  prof.apply_emotional_stimulus(-0.7, 0.85, 0.2, 0.6);
  SAIGE_CHECK(prof.emotions().fear > 0.8);
  SAIGE_CHECK(prof.evaluate(EnvironmentalCondition{}, 1).action ==
              ContextualAction::FleeHazard);

  prof.step(20.0, EnvironmentalCondition{});
  SAIGE_CHECK(prof.emotions().fear < 0.2);
}

}  // namespace

int main() {
  test_individual_habit_variation_is_deterministic();
  test_experience_memory_and_environmental_reactions();
  test_emotional_stimulus_and_decay();
  std::cout << "npc_psychology_test: ALL 3 SUITES PASSED\n";
  return 0;
}
