#include "saige/social_simulation.hpp"

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

using namespace saige::social;
using namespace saige::world_sim;
using namespace saige::psychology;
using namespace saige::persistent_world;

namespace {

void test_relationship_progression_and_persistence() {
  SocialNetwork net;
  NpcAgent a{};
  a.id = 1;
  a.personality.sociability = 0.8;
  NpcAgent b{};
  b.id = 2;
  NpcPsychologyProfile psych_a(1);
  EnvironmentalCondition env{};

  // Repeated positive interactions evolve Acquaintance -> Friend
  for (int i = 0; i < 8; ++i) {
    SAIGE_CHECK(net.apply_encounter(a, b, psych_a, env, static_cast<double>(i) * 15.0));
  }
  const auto* edge = net.find_relationship(1, 2);
  SAIGE_CHECK(edge != nullptr);
  SAIGE_CHECK(edge->interaction_count == 8);
  SAIGE_CHECK(edge->kind == RelationshipKind::Friend);
  SAIGE_CHECK(edge->affinity >= 0.45f);

  PersistentWorldStore store;
  net.sync_to_persistent_store(store);
  const auto blob = store.serialize();

  PersistentWorldStore restored_store;
  SAIGE_CHECK(restored_store.deserialize(blob));

  SocialNetwork restored_net;
  restored_net.load_from_persistent_store(restored_store);
  SAIGE_CHECK(restored_net.edge_count() == 1);
  SAIGE_CHECK(restored_net.find_relationship(1, 2)->kind == RelationshipKind::Friend);
}

void test_helping_avoiding_and_confronting() {
  SocialNetwork net;
  SAIGE_CHECK(net.set_relationship({1, 2, RelationshipKind::Family, 0.8f, 0.9f, 0.9f, 5, -50.0}));
  SAIGE_CHECK(net.set_relationship({3, 4, RelationshipKind::Rival, -0.7f, 0.5f, -0.6f, 3, -50.0}));

  NpcAgent helper{};
  helper.id = 1;
  NpcAgent injured_family{};
  injured_family.id = 2;
  injured_family.drives.safety = 0.2;
  NpcPsychologyProfile psych_helper(1);

  const auto help_dec = net.evaluate_encounter(helper, injured_family, psych_helper, EnvironmentalCondition{}, 10.0);
  SAIGE_CHECK(help_dec.action == SocialAction::HelpAlly);

  NpcAgent angry_rival{};
  angry_rival.id = 3;
  angry_rival.personality.aggression = 0.8;
  NpcAgent rival_target{};
  rival_target.id = 4;
  NpcPsychologyProfile psych_rival(3);
  psych_rival.apply_emotional_stimulus(-0.5, 0.0, 0.7, 0.3);

  const auto conf_dec = net.evaluate_encounter(angry_rival, rival_target, psych_rival, EnvironmentalCondition{}, 10.0);
  SAIGE_CHECK(conf_dec.action == SocialAction::Confront);
}

void test_social_gathering_and_group_formation() {
  SocialNetwork net;
  const WorldPosition plaza = WorldPosition::from_world_double({50.0, 50.0, 0.0});
  SAIGE_CHECK(net.add_gathering({10, 5, plaza, 1080, 1260, {1, 2, 3}}));

  std::vector<NpcAgent> npcs(3);
  npcs[0].id = 1;
  npcs[1].id = 2;
  npcs[2].id = 3;

  const std::uint32_t scheduled = net.apply_gathering_schedules(npcs, 1140);
  SAIGE_CHECK(scheduled == 3);
  for (const auto& npc : npcs) {
    SAIGE_CHECK(npc.current_nav_intent.intent == NpcIntent::Socialize);
    SAIGE_CHECK(npc.current_nav_intent.has_destination);
  }
  // Distinct formation slots around gathering center (no stacking)
  SAIGE_CHECK(WorldPosition::distance(npcs[0].current_nav_intent.target_position,
                                      npcs[1].current_nav_intent.target_position) > 1.0);
}

}  // namespace

void test_10km_separation_blocks_encounter_and_flee_priority() {
  SocialNetwork net;
  NpcAgent a{};
  a.id = 1;
  a.position = WorldPosition::from_world_double({0.0, 0.0, 0.0});
  a.current_nav_intent.intent = NpcIntent::Flee;
  a.current_nav_intent.urgent = true;

  NpcAgent b{};
  b.id = 2;
  b.position = WorldPosition::from_world_double({10000.0, 0.0, 0.0});  // 10 km away
  NpcPsychologyProfile psych_a(1);

  SAIGE_CHECK(net.evaluate_encounter(a, b, psych_a, EnvironmentalCondition{}, 10.0).action ==
              SocialAction::None);
  SAIGE_CHECK(!net.apply_encounter(a, b, psych_a, EnvironmentalCondition{}, 10.0));

  // Urgent Flee must NOT be overridden by social gathering schedule
  SAIGE_CHECK(net.add_gathering({20, 5, WorldPosition{}, 1080, 1260, {1, 2}}));
  std::vector<NpcAgent> group{a, b};
  const std::uint32_t scheduled = net.apply_gathering_schedules(group, 1140);
  SAIGE_CHECK(scheduled == 1);  // Only NPC 2 scheduled
  SAIGE_CHECK(group[0].current_nav_intent.intent == NpcIntent::Flee);
  SAIGE_CHECK(group[0].current_nav_intent.urgent);
}

int main() {
  test_relationship_progression_and_persistence();
  test_helping_avoiding_and_confronting();
  test_social_gathering_and_group_formation();
  test_10km_separation_blocks_encounter_and_flee_priority();
  std::cout << "social_simulation_test: ALL 4 SUITES PASSED\n";
  return 0;
}
