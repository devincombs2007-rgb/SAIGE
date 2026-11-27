// Regression tests for the defects found in World Simulation Build 010 on receipt (2026-11-27, upstream/gemini/PROVENANCE.md). Each case failed on the
// delivered code (measured with a probe against upstream/gemini/world_simulation_build010) and passes on the integrated, corrected copy.
#include "saige/local_economy.hpp"
#include "saige/persistent_world.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace saige;
namespace pw = saige::persistent_world;

namespace {
int g_failures = 0, g_checks = 0;
void check(bool ok, const char* what)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

// A save written by the delivered Build 010 serializer (WLD2 v2): two NPCs, one relationship, one business, one event.
const std::uint8_t kBuild010V2Save[] = {
0x32, 0x44, 0x4c, 0x57, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x20, 0x41, 0x00, 0x00, 0xa0, 0x40, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe8, 0x03, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f,
0x9a, 0x99, 0x59, 0x3f, 0x00, 0x00, 0x80, 0x3e, 0xcd, 0xcc, 0xcc, 0x3d, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xa0, 0x41,
0x00, 0x00, 0xa0, 0x40, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd0, 0x07, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00, 0x3f, 0x9a, 0x99,
0x59, 0x3f, 0x00, 0x00, 0x80, 0x3e, 0xcd, 0xcc, 0xcc, 0x3d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3f, 0x00,
0x00, 0x80, 0x3e, 0x00, 0x00, 0x40, 0x3f, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x01, 0x38, 0x04,
0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29, 0x40, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0xf0, 0x3f, 0xc4, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3e, 0x40, 0x9a, 0x99, 0x99,
0x99, 0x99, 0x99, 0xe9, 0x3f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x56, 0x40, 0x46, 0xc4, 0x3f,
0xcd, 
};

std::int64_t wage_paid(double hz, double seconds, std::int64_t wage_per_minute)
{
    economy::EconomySystem eco;
    economy::HouseholdAccount h;
    h.npc_id = 1;
    eco.register_household(h);
    economy::EconomyBusiness b;
    b.venue_id = 7;
    b.cash_cents = 1000000;
    eco.register_business(b);
    economy::EmploymentContract c;
    c.npc_id = 1;
    c.employer_venue = 7;
    c.wage_per_minute_cents = wage_per_minute;
    eco.assign_employment(c);
    world_sim::NpcAgent worker;
    worker.id = 1;
    worker.position = b.anchor;
    const std::vector<world_sim::NpcAgent> npcs{worker};
    const int ticks = static_cast<int>(std::lround(seconds * hz));
    for (int t = 0; t < ticks; ++t) eco.step(static_cast<std::uint64_t>(t + 1), 1.0 / hz, 600, npcs);
    return eco.find_household(1)->balance_cents;
}

void test_wages_do_not_depend_on_the_tick_rate()
{
    // Build 010 paid at least a cent per tick: 60 cents at 1 Hz and 3600 at 60 Hz for one minute at 25 cents a minute.
    for (const double hz : {1.0, 10.0, 60.0, 144.0}) {
        const std::int64_t one_minute = wage_paid(hz, 60.0, 25);
        const std::int64_t ten_minutes = wage_paid(hz, 600.0, 25);
        std::printf("  wage at %g Hz: 1 min -> %lld cents, 10 min -> %lld cents\n", hz, static_cast<long long>(one_minute), static_cast<long long>(ten_minutes));
        check(one_minute == 25, "one minute at 25 c/min pays 25 cents at every tick rate");
        check(ten_minutes == 250, "ten minutes pay 250 cents at every tick rate");
    }
    check(wage_paid(60.0, 120.0, 0) == 0, "a zero wage pays nothing");
}

void test_amounts_are_bounded_and_cannot_overflow()
{
    economy::EconomySystem eco;
    economy::HouseholdAccount h;
    h.npc_id = 1;
    h.balance_cents = 10;
    check(eco.register_household(h), "a valid household registers");
    economy::EconomyBusiness huge;
    huge.venue_id = 7;
    huge.unit_price_cents = std::int64_t{1} << 62;  // Build 010 accepted it, then 4 units wrapped to a negative cost and the buyer gained money
    check(!eco.register_business(huge), "a price beyond kMaxUnitPriceCents is refused");
    economy::EconomyBusiness ok;
    ok.venue_id = 8;
    ok.unit_price_cents = economy::kMaxUnitPriceCents;
    ok.inventory_units = 50;
    check(eco.register_business(ok), "the largest allowed price registers");
    const std::int64_t before = eco.total_money_cents();
    check(eco.attempt_purchase(1, 8, economy::kMaxPurchaseUnits + 1, 600) == economy::PurchaseStatus::InvalidQuantity, "too large a quantity is refused");
    check(eco.attempt_purchase(1, 8, 0, 600) == economy::PurchaseStatus::InvalidQuantity, "a zero quantity is refused");
    check(eco.attempt_purchase(1, 8, 4, 600) == economy::PurchaseStatus::InsufficientFunds, "an expensive purchase is refused for lack of funds, not wrapped");
    check(eco.total_money_cents() == before && eco.find_household(1)->balance_cents == 10, "no refused purchase changes any balance");
    bool threw = false;
    try {
        economy::EconomySystem bad(-1);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    check(threw, "a negative treasury is refused");
}

void test_registration_refuses_nonsense()
{
    economy::EconomySystem eco;
    const auto refuses = [&](void (*mutate)(economy::EconomyBusiness&), const char* what) {
        economy::EconomyBusiness b;
        b.venue_id = 9;
        mutate(b);
        check(!eco.register_business(b), what);
    };
    refuses([](economy::EconomyBusiness& b) { b.open_minute = 5000; }, "an opening minute past the day is refused");
    refuses([](economy::EconomyBusiness& b) { b.close_minute = 1440; }, "a closing minute of 1440 is refused");
    refuses([](economy::EconomyBusiness& b) { b.cash_cents = -100; }, "negative cash is refused");
    refuses([](economy::EconomyBusiness& b) { b.inventory_units = 500; b.max_inventory = 10; }, "stock above the maximum is refused");
    refuses([](economy::EconomyBusiness& b) { b.reorder_threshold = 200; b.max_inventory = 100; }, "a reorder threshold above the maximum is refused");
    economy::EmploymentContract c;
    c.npc_id = 1;
    c.employer_venue = 7;
    c.shift_end_min = 2000;
    check(!eco.assign_employment(c), "a shift ending past the day is refused");
}

void test_store_refuses_invalid_records()
{
    pw::PersistentWorldStore s;
    pw::PersistentNpcRecord npc;
    npc.id = 9;
    npc.position.local.x = std::numeric_limits<float>::quiet_NaN();
    check(!s.upsert_npc(npc), "an NPC with a NaN position is refused");
    npc.position.local.x = 0.0f;
    npc.hunger = 7.0f;
    check(!s.upsert_npc(npc), "an NPC with hunger 7 is refused");
    npc.hunger = 0.5f;
    check(s.upsert_npc(npc), "a valid NPC is stored");
    pw::RelationshipRecord rel;
    rel.from_id = 1;
    rel.to_id = 2;
    rel.affinity = std::numeric_limits<float>::infinity();
    check(!s.upsert_relationship(rel), "an infinite affinity is refused");
    pw::PersistentEventRecord e;
    e.id = 3;
    e.radius_m = -5.0;
    check(!s.add_event(e), "a negative event radius is refused");
    pw::BusinessRecord b;
    b.venue_id = 4;
    b.unit_price = std::numeric_limits<double>::quiet_NaN();
    check(!s.upsert_business(b), "a NaN price is refused");
}

void test_ledger_survives_a_save()
{
    // Build 010 persisted neither the tills nor the treasury: 125000 cents before a save became 202600 after loading it.
    const auto make = [] {
        economy::EconomySystem eco(100000);
        economy::HouseholdAccount h;
        h.npc_id = 1;
        h.balance_cents = 5000;
        eco.register_household(h);
        economy::EconomyBusiness biz;
        biz.venue_id = 7;
        biz.cash_cents = 20000;
        biz.inventory_units = 30;
        eco.register_business(biz);
        return eco;
    };
    economy::EconomySystem eco = make();
    check(eco.attempt_purchase(1, 7, 2, 600) == economy::PurchaseStatus::Success, "the purchase succeeds");
    pw::PersistentWorldStore store;
    pw::PersistentNpcRecord r;
    r.id = 1;
    check(store.upsert_npc(r), "the buyer's record is stored");
    eco.sync_to_persistent_store(store);
    pw::PersistentWorldStore loaded;
    check(loaded.deserialize(store.serialize()), "the save loads");
    economy::EconomySystem eco2(0);
    economy::HouseholdAccount h2;
    h2.npc_id = 1;
    eco2.register_household(h2);
    economy::EconomyBusiness biz2;
    biz2.venue_id = 7;
    eco2.register_business(biz2);
    eco2.load_from_persistent_store(loaded);
    std::printf("  ledger: %lld cents before the save, %lld after loading it\n", static_cast<long long>(eco.total_money_cents()), static_cast<long long>(eco2.total_money_cents()));
    check(eco2.total_money_cents() == eco.total_money_cents(), "a save and load conserves the total money");
    check(eco2.find_business(7)->cash_cents == eco.find_business(7)->cash_cents, "the till is restored");
    check(eco2.supplier_treasury_cents() == eco.supplier_treasury_cents(), "the treasury is restored");
}

void test_build010_saves_still_load()
{
    pw::PersistentWorldStore s;
    const std::vector<std::uint8_t> blob(std::begin(kBuild010V2Save), std::end(kBuild010V2Save));
    check(s.deserialize(blob), "a Build 010 (v2) save loads");
    check(s.npc_count() == 2 && s.relationship_count() == 1 && s.business_count() == 1 && s.event_count() == 1, "every record of the v2 save is present");
    const pw::RelationshipRecord* rel = s.find_relationship(1, 2);
    check(rel != nullptr && rel->trust == 0.75f && rel->interaction_count == 3, "the v2 relationship fields survive");
    const pw::BusinessRecord* b = s.find_business(7);
    check(b != nullptr && b->unit_price == 12.5 && b->cumulative_revenue_cents == 2500 && b->cash_cents == 0, "the v2 business loads with no till (v2 did not store one)");
    check(!s.has_economy_treasury(), "a v2 save carries no treasury");
    // A loaded v2 save writes v3 and loads back identically.
    pw::PersistentWorldStore again;
    check(again.deserialize(s.serialize()) && again.serialize() == s.serialize(), "the v2 save re-saved as v3 round-trips byte for byte");
    // Truncated or trailing bytes are refused and leave the store as it was.
    std::vector<std::uint8_t> shorter(blob.begin(), blob.end() - 1);
    pw::PersistentWorldStore untouched;
    check(!untouched.deserialize(shorter) && untouched.npc_count() == 0, "a truncated save is refused and changes nothing");
}
}  // namespace

int main()
{
    test_wages_do_not_depend_on_the_tick_rate();
    test_amounts_are_bounded_and_cannot_overflow();
    test_registration_refuses_nonsense();
    test_store_refuses_invalid_records();
    test_ledger_survives_a_save();
    test_build010_saves_still_load();
    std::printf("world_sim_regression: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
