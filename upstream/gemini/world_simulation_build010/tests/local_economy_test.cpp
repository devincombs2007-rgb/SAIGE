#include "saige/local_economy.hpp"

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

using namespace saige::economy;
using namespace saige::world_sim;
using namespace saige::psychology;
using namespace saige::persistent_world;

namespace {

void test_wage_attendance_and_duplicate_tick_guard() {
  EconomySystem econ(100000);
  const std::int64_t initial_total = econ.total_money_cents();

  SAIGE_CHECK(econ.register_household({1, 0, 5000, 0}));
  EconomyBusiness shop{};
  shop.venue_id = 10;
  shop.anchor = WorldPosition::from_world_double({20.0, 0.0, 0.0});
  shop.cash_cents = 50000;
  SAIGE_CHECK(econ.register_business(shop));
  SAIGE_CHECK(econ.assign_employment({1, 10, OccupationKind::Shopkeeper, 25, 480, 1020}));

  NpcAgent worker{};
  worker.id = 1;
  worker.position = WorldPosition::from_world_double({80.0, 0.0, 0.0});  // Absent
  std::vector<NpcAgent> npcs{worker};

  econ.step(1, 60.0, 540, npcs);
  SAIGE_CHECK(econ.find_household(1)->balance_cents == 0);  // Absent -> 0 pay

  npcs[0].position = WorldPosition::from_world_double({21.0, 0.0, 0.0});  // Present
  econ.step(2, 60.0, 540, npcs);
  SAIGE_CHECK(econ.find_household(1)->balance_cents == 25);

  // Duplicate step with same tick_id=2 must NOT double-pay
  econ.step(2, 60.0, 540, npcs);
  SAIGE_CHECK(econ.find_household(1)->balance_cents == 25);
  SAIGE_CHECK(econ.total_money_cents() == initial_total + 50000);
}

void test_purchase_insufficient_funds_exhaustion_and_delivery() {
  EconomySystem econ(1000000);
  econ.register_delivery_vehicle(900);
  SAIGE_CHECK(econ.register_household({1, 500, 5000, 0}));   // Poor ($5.00)
  SAIGE_CHECK(econ.register_household({2, 5000, 5000, 0}));  // Solvent ($50.00)

  EconomyBusiness cafe{};
  cafe.venue_id = 1;
  cafe.anchor = WorldPosition::from_world_double({10.0, 0.0, 0.0});
  cafe.cash_cents = 100000;
  cafe.inventory_units = 2;
  cafe.max_inventory = 10;
  cafe.reorder_threshold = 1;
  cafe.unit_price_cents = 1200;
  cafe.wholesale_unit_cost_cents = 500;
  SAIGE_CHECK(econ.register_business(cafe));

  const std::int64_t total_before = econ.total_money_cents();

  // Insufficient funds
  SAIGE_CHECK(econ.attempt_purchase(1, 1, 1, 600) == PurchaseStatus::InsufficientFunds);

  // Solvent buyer buys all 2 units -> inventory exhausted -> temporarily closed
  NpcAgent buyer{};
  buyer.id = 2;
  buyer.drives.hunger = 0.9;
  NpcPsychologyProfile psych(2);
  SAIGE_CHECK(econ.attempt_purchase(2, 1, 2, 600, &buyer, &psych, 10.0) ==
              PurchaseStatus::Success);
  SAIGE_CHECK(econ.find_household(2)->balance_cents == 2600);
  SAIGE_CHECK(econ.find_business(1)->temporarily_closed);
  SAIGE_CHECK(buyer.drives.hunger < 0.5);
  SAIGE_CHECK(psych.venue_affinity(1) > 0.0);

  // Attempting another purchase while closed fails
  SAIGE_CHECK(econ.attempt_purchase(2, 1, 1, 600) == PurchaseStatus::BusinessClosed);

  // Step dispatches delivery vehicle 900; completing delivery restocks & reopens
  econ.step(1, 1.0, 600, {});
  SAIGE_CHECK(econ.delivery_orders().size() == 1);
  SAIGE_CHECK(econ.delivery_orders()[0].vehicle_id == 900);
  SAIGE_CHECK(econ.complete_delivery(econ.delivery_orders()[0].order_id));
  SAIGE_CHECK(!econ.find_business(1)->temporarily_closed);
  SAIGE_CHECK(econ.find_business(1)->inventory_units == 10);

  // Exact integer currency conservation across all transactions
  SAIGE_CHECK(econ.total_money_cents() == total_before);
}

void test_economic_destinations_and_save_load_consistency() {
  EconomySystem econ(100000);
  SAIGE_CHECK(econ.register_household({1, 3000, 5000, 0}));
  EconomyBusiness b1{};
  b1.venue_id = 10;
  b1.anchor = WorldPosition::from_world_double({5.0, 0.0, 0.0});
  b1.inventory_units = 0;
  b1.temporarily_closed = true;
  EconomyBusiness b2{};
  b2.venue_id = 20;
  b2.anchor = WorldPosition::from_world_double({30.0, 0.0, 0.0});
  b2.inventory_units = 10;
  b2.unit_price_cents = 1000;
  SAIGE_CHECK(econ.register_business(b1));
  SAIGE_CHECK(econ.register_business(b2));

  NpcAgent hungry{};
  hungry.id = 1;
  hungry.drives.hunger = 0.85;
  hungry.position = WorldPosition::from_world_double({0.0, 0.0, 0.0});
  // Skips closed/empty b1 (closer) and selects stocked b2
  SAIGE_CHECK(econ.choose_economic_destination(hungry, 600) == 20);

  PersistentWorldStore store;
  store.upsert_npc({1, hungry.position, SimLodTier::Active, OccupationKind::Unemployed,
                    0, 0, 0, 3000});
  econ.sync_to_persistent_store(store);
  const auto blob = store.serialize();

  PersistentWorldStore restored_store;
  SAIGE_CHECK(restored_store.deserialize(blob));
  EconomySystem restored_econ(100000);
  restored_econ.register_household({1, 0, 5000, 0});
  restored_econ.register_business({20});
  restored_econ.load_from_persistent_store(restored_store);
  SAIGE_CHECK(restored_econ.find_household(1)->balance_cents == 3000);
  SAIGE_CHECK(restored_econ.find_business(20)->inventory_units == 10);
}

}  // namespace

int main() {
  test_wage_attendance_and_duplicate_tick_guard();
  test_purchase_insufficient_funds_exhaustion_and_delivery();
  test_economic_destinations_and_save_load_consistency();
  std::cout << "local_economy_test: ALL 3 SUITES PASSED\n";
  return 0;
}
