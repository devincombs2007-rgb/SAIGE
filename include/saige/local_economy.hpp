#pragma once

#include "saige/npc_psychology.hpp"
#include "saige/npc_simulation.hpp"
#include "saige/persistent_world.hpp"

#include <cstdint>
#include <vector>

namespace saige::economy {

// Bounds (SAIGE fix, 2026-11-27): Build 010 multiplied unbounded int64 amounts, so a large price overflowed (undefined behaviour) and a wrapped,
// negative cost made a purchase create money. Registration now refuses amounts outside these bounds, and every product is checked.
inline constexpr std::int64_t kMaxAccountCents = 1'000'000'000'000'000;  // 10^15 cents per account or till
inline constexpr std::int64_t kMaxUnitPriceCents = 1'000'000'000;        // 10^9 cents per unit
inline constexpr std::int64_t kMaxWagePerMinuteCents = 10'000'000;
inline constexpr std::uint32_t kMaxInventoryUnits = 10'000'000;
inline constexpr std::uint32_t kMaxPurchaseUnits = 100'000;

struct HouseholdAccount {
  world_sim::EntityId npc_id = world_sim::kInvalidEntityId;
  std::int64_t balance_cents = 0;
  std::int64_t daily_budget_cents = 5000;
  std::uint64_t last_paid_tick = 0;
};

struct EmploymentContract {
  world_sim::EntityId npc_id = world_sim::kInvalidEntityId;
  world_sim::VenueId employer_venue = world_sim::kInvalidVenueId;
  world_sim::OccupationKind role = world_sim::OccupationKind::Shopkeeper;
  std::int64_t wage_per_minute_cents = 25;
  std::uint16_t shift_start_min = 480;
  std::uint16_t shift_end_min = 1020;
};

struct EconomyBusiness {
  world_sim::VenueId venue_id = world_sim::kInvalidVenueId;
  world_sim::VenueCategory category = world_sim::VenueCategory::Commercial;
  world_sim::WorldPosition anchor{};
  std::uint16_t open_minute = 480;
  std::uint16_t close_minute = 1080;
  std::int64_t cash_cents = 100000;
  std::uint32_t inventory_units = 50;
  std::uint32_t max_inventory = 100;
  std::uint32_t reorder_threshold = 15;
  std::int64_t unit_price_cents = 1200;
  std::int64_t wholesale_unit_cost_cents = 600;
  std::uint64_t assigned_delivery_vehicle = 0;
  bool delivery_pending = false;
  bool temporarily_closed = false;
  std::uint64_t revenue_cents = 0;
  std::uint64_t expense_cents = 0;
  std::uint64_t unmet_demand_count = 0;

  [[nodiscard]] bool is_open(std::uint16_t minute_of_day) const noexcept;
};

struct DeliveryOrder {
  std::uint32_t order_id = 0;
  world_sim::VenueId target_venue = world_sim::kInvalidVenueId;
  std::uint64_t vehicle_id = 0;
  std::uint32_t units = 0;
  std::int64_t wholesale_cost_cents = 0;
  bool completed = false;
};

enum class PurchaseStatus : std::uint8_t {
  Success = 0,
  UnknownAccountOrBusiness = 1,
  BusinessClosed = 2,
  OutOfStock = 3,
  InsufficientFunds = 4,
  InvalidQuantity = 5,  ///< zero, or more than kMaxPurchaseUnits
};

class EconomySystem {
 public:
  /// Throws std::invalid_argument for a treasury outside [0, kMaxAccountCents].
  explicit EconomySystem(std::int64_t initial_supplier_treasury_cents = 10000000);

  bool register_household(const HouseholdAccount& acc);
  bool register_business(const EconomyBusiness& biz);
  bool assign_employment(const EmploymentContract& contract);
  void register_delivery_vehicle(std::uint64_t vehicle_id);

  [[nodiscard]] const HouseholdAccount* find_household(
      world_sim::EntityId npc_id) const noexcept;
  [[nodiscard]] const EconomyBusiness* find_business(
      world_sim::VenueId venue_id) const noexcept;
  [[nodiscard]] const std::vector<DeliveryOrder>& delivery_orders() const noexcept {
    return deliveries_;
  }

  // Advances attendance-verified wage payouts and automatic delivery dispatches.
  void step(std::uint64_t tick_id, double dt_seconds,
            std::uint16_t minute_of_day,
            const std::vector<world_sim::NpcAgent>& npcs);

  // Executes atomic integer-cents purchase; updates buyer hunger & psychology.
  PurchaseStatus attempt_purchase(world_sim::EntityId buyer_id,
                                  world_sim::VenueId venue_id,
                                  std::uint32_t quantity,
                                  std::uint16_t minute_of_day,
                                  world_sim::NpcAgent* buyer_npc = nullptr,
                                  psychology::NpcPsychologyProfile* buyer_psych = nullptr,
                                  double current_time_sec = 0.0);

  // Completes a delivery upon vehicle arrival, restocking inventory & reopening.
  bool complete_delivery(std::uint32_t order_id);

  // Selects an open, stocked, affordable venue (or workplace if low funds).
  [[nodiscard]] world_sim::VenueId choose_economic_destination(
      const world_sim::NpcAgent& npc,
      std::uint16_t minute_of_day) const noexcept;

  void sync_to_persistent_store(
      persistent_world::PersistentWorldStore& store) const;
  void load_from_persistent_store(
      const persistent_world::PersistentWorldStore& store);

  // Total currency across all households + businesses + supplier treasury.
  [[nodiscard]] std::int64_t total_money_cents() const noexcept;
  [[nodiscard]] std::int64_t supplier_treasury_cents() const noexcept {
    return supplier_treasury_cents_;
  }

 private:
  std::int64_t supplier_treasury_cents_ = 0;
  std::uint32_t next_order_id_ = 1;
  std::vector<HouseholdAccount> households_;
  std::vector<EconomyBusiness> businesses_;
  std::vector<EmploymentContract> contracts_;
  std::vector<double> wage_accrued_cents_;  ///< per contract: earned but not yet paid fractions of a cent (exact accrual across ticks; SAIGE fix)
  std::vector<std::uint64_t> delivery_vehicles_;
  std::vector<DeliveryOrder> deliveries_;
};

}  // namespace saige::economy
