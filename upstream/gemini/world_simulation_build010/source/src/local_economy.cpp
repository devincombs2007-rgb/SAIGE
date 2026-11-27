#include "saige/local_economy.hpp"

#include <algorithm>
#include <cmath>

namespace saige::economy {

namespace {

[[nodiscard]] bool in_minute_window(std::uint16_t m, std::uint16_t start,
                                    std::uint16_t end) noexcept {
  if (start == end) return true;
  if (start < end) return m >= start && m < end;
  return m >= start || m < end;
}

}  // namespace

bool EconomyBusiness::is_open(std::uint16_t minute_of_day) const noexcept {
  return !temporarily_closed &&
         in_minute_window(minute_of_day, open_minute, close_minute);
}

EconomySystem::EconomySystem(std::int64_t initial_supplier_treasury_cents)
    : supplier_treasury_cents_(initial_supplier_treasury_cents) {}

bool EconomySystem::register_household(const HouseholdAccount& acc) {
  if (acc.npc_id == world_sim::kInvalidEntityId || acc.balance_cents < 0) {
    return false;
  }
  for (const auto& h : households_) {
    if (h.npc_id == acc.npc_id) return false;
  }
  households_.push_back(acc);
  return true;
}

bool EconomySystem::register_business(const EconomyBusiness& biz) {
  if (biz.venue_id == world_sim::kInvalidVenueId || biz.unit_price_cents < 0 ||
      biz.wholesale_unit_cost_cents < 0) {
    return false;
  }
  for (const auto& b : businesses_) {
    if (b.venue_id == biz.venue_id) return false;
  }
  businesses_.push_back(biz);
  return true;
}

bool EconomySystem::assign_employment(const EmploymentContract& contract) {
  if (contract.npc_id == world_sim::kInvalidEntityId ||
      contract.employer_venue == world_sim::kInvalidVenueId ||
      contract.wage_per_minute_cents < 0) {
    return false;
  }
  for (auto& c : contracts_) {
    if (c.npc_id == contract.npc_id) {
      c = contract;
      return true;
    }
  }
  contracts_.push_back(contract);
  return true;
}

void EconomySystem::register_delivery_vehicle(std::uint64_t vehicle_id) {
  if (vehicle_id == 0) return;
  if (std::find(delivery_vehicles_.begin(), delivery_vehicles_.end(),
                vehicle_id) == delivery_vehicles_.end()) {
    delivery_vehicles_.push_back(vehicle_id);
  }
}

const HouseholdAccount* EconomySystem::find_household(
    world_sim::EntityId npc_id) const noexcept {
  for (const auto& h : households_) {
    if (h.npc_id == npc_id) return &h;
  }
  return nullptr;
}

const EconomyBusiness* EconomySystem::find_business(
    world_sim::VenueId venue_id) const noexcept {
  for (const auto& b : businesses_) {
    if (b.venue_id == venue_id) return &b;
  }
  return nullptr;
}

void EconomySystem::step(std::uint64_t tick_id, double dt_seconds,
                         std::uint16_t minute_of_day,
                         const std::vector<world_sim::NpcAgent>& npcs) {
  if (!std::isfinite(dt_seconds) || dt_seconds <= 0.0) return;

  // 1. Pay wages only for physical attendance during scheduled shift (no duplicate ticks)
  for (const auto& c : contracts_) {
    if (!in_minute_window(minute_of_day, c.shift_start_min, c.shift_end_min)) {
      continue;
    }
    const world_sim::NpcAgent* worker = nullptr;
    for (const auto& n : npcs) {
      if (n.id == c.npc_id) {
        worker = &n;
        break;
      }
    }
    if (worker == nullptr) continue;

    HouseholdAccount* hh = nullptr;
    for (auto& h : households_) {
      if (h.npc_id == c.npc_id) {
        hh = &h;
        break;
      }
    }
    EconomyBusiness* biz = nullptr;
    for (auto& b : businesses_) {
      if (b.venue_id == c.employer_venue) {
        biz = &b;
        break;
      }
    }
    if (hh == nullptr || biz == nullptr) continue;
    if (hh->last_paid_tick == tick_id) continue;  // Prevent duplicate payment on same tick

    const double dist =
        world_sim::WorldPosition::distance(worker->position, biz->anchor);
    if (dist <= 6.0) {
      const std::int64_t pay = std::max<std::int64_t>(
          1, static_cast<std::int64_t>(
                 std::llround((dt_seconds / 60.0) *
                              static_cast<double>(c.wage_per_minute_cents))));
      if (biz->cash_cents >= pay) {
        biz->cash_cents -= pay;
        biz->expense_cents += static_cast<std::uint64_t>(pay);
        hh->balance_cents += pay;
        hh->last_paid_tick = tick_id;
      }
    }
  }

  // 2. Trigger supply delivery orders when inventory falls to reorder threshold
  for (auto& biz : businesses_) {
    if (biz.inventory_units <= biz.reorder_threshold && !biz.delivery_pending) {
      const std::uint32_t order_units =
          (biz.max_inventory > biz.inventory_units)
              ? (biz.max_inventory - biz.inventory_units)
              : 0;
      if (order_units > 0) {
        const std::int64_t cost =
            static_cast<std::int64_t>(order_units) * biz.wholesale_unit_cost_cents;
        if (biz.cash_cents >= cost) {
          std::uint64_t veh = biz.assigned_delivery_vehicle;
          if (veh == 0 && !delivery_vehicles_.empty()) {
            veh = delivery_vehicles_[deliveries_.size() % delivery_vehicles_.size()];
          }
          if (veh != 0) {
            deliveries_.push_back(
                {next_order_id_++, biz.venue_id, veh, order_units, cost, false});
            biz.delivery_pending = true;
          }
        }
      }
    }
  }
}

PurchaseStatus EconomySystem::attempt_purchase(
    world_sim::EntityId buyer_id, world_sim::VenueId venue_id,
    std::uint32_t quantity, std::uint16_t minute_of_day,
    world_sim::NpcAgent* buyer_npc, psychology::NpcPsychologyProfile* buyer_psych,
    double current_time_sec) {
  if (quantity == 0) return PurchaseStatus::UnknownAccountOrBusiness;

  HouseholdAccount* hh = nullptr;
  for (auto& h : households_) {
    if (h.npc_id == buyer_id) {
      hh = &h;
      break;
    }
  }
  EconomyBusiness* biz = nullptr;
  for (auto& b : businesses_) {
    if (b.venue_id == venue_id) {
      biz = &b;
      break;
    }
  }
  if (hh == nullptr || biz == nullptr) {
    return PurchaseStatus::UnknownAccountOrBusiness;
  }
  if (!biz->is_open(minute_of_day)) {
    ++biz->unmet_demand_count;
    return PurchaseStatus::BusinessClosed;
  }
  if (biz->inventory_units < quantity) {
    ++biz->unmet_demand_count;
    if (biz->inventory_units == 0) {
      biz->temporarily_closed = true;
    }
    return PurchaseStatus::OutOfStock;
  }

  const std::int64_t total_cost =
      static_cast<std::int64_t>(quantity) * biz->unit_price_cents;
  if (hh->balance_cents < total_cost) {
    return PurchaseStatus::InsufficientFunds;
  }

  hh->balance_cents -= total_cost;
  biz->cash_cents += total_cost;
  biz->revenue_cents += static_cast<std::uint64_t>(total_cost);
  biz->inventory_units -= quantity;
  if (biz->inventory_units == 0) {
    biz->temporarily_closed = true;
  }

  if (buyer_npc != nullptr) {
    buyer_npc->drives.hunger = std::max(0.0, buyer_npc->drives.hunger - 0.5);
  }
  if (buyer_psych != nullptr) {
    buyer_psych->record_experience(
        {venue_id, world_sim::kInvalidEntityId, 0.4, current_time_sec});
  }
  return PurchaseStatus::Success;
}

bool EconomySystem::complete_delivery(std::uint32_t order_id) {
  for (auto& d : deliveries_) {
    if (d.order_id == order_id && !d.completed) {
      for (auto& biz : businesses_) {
        if (biz.venue_id == d.target_venue) {
          if (biz.cash_cents < d.wholesale_cost_cents) return false;
          biz.cash_cents -= d.wholesale_cost_cents;
          biz.expense_cents += static_cast<std::uint64_t>(d.wholesale_cost_cents);
          supplier_treasury_cents_ += d.wholesale_cost_cents;
          biz.inventory_units =
              std::min(biz.max_inventory, biz.inventory_units + d.units);
          biz.delivery_pending = false;
          if (biz.inventory_units > 0) {
            biz.temporarily_closed = false;
          }
          d.completed = true;
          return true;
        }
      }
    }
  }
  return false;
}

world_sim::VenueId EconomySystem::choose_economic_destination(
    const world_sim::NpcAgent& npc,
    std::uint16_t minute_of_day) const noexcept {
  const HouseholdAccount* hh = find_household(npc.id);
  const std::int64_t funds = hh ? hh->balance_cents : 0;

  if (npc.drives.hunger > 0.65) {
    double best_dist = 1e18;
    world_sim::VenueId best_id = world_sim::kInvalidVenueId;
    for (const auto& biz : businesses_) {
      if (!biz.is_open(minute_of_day) || biz.inventory_units == 0) continue;
      if (funds < biz.unit_price_cents) continue;
      const double d = world_sim::WorldPosition::distance(npc.position, biz.anchor);
      if (d < best_dist) {
        best_dist = d;
        best_id = biz.venue_id;
      }
    }
    if (best_id != world_sim::kInvalidVenueId) return best_id;
  }

  for (const auto& c : contracts_) {
    if (c.npc_id == npc.id &&
        in_minute_window(minute_of_day, c.shift_start_min, c.shift_end_min)) {
      return c.employer_venue;
    }
  }
  return npc.persistent_memory.home_venue;
}

void EconomySystem::sync_to_persistent_store(
    persistent_world::PersistentWorldStore& store) const {
  for (const auto& hh : households_) {
    store.set_npc_balance(hh.npc_id, hh.balance_cents);
  }
  for (const auto& biz : businesses_) {
    persistent_world::BusinessRecord rec{};
    rec.venue_id = biz.venue_id;
    rec.category = biz.category;
    rec.anchor = biz.anchor;
    rec.open_minute = biz.open_minute;
    rec.close_minute = biz.close_minute;
    rec.inventory_units = biz.inventory_units;
    rec.unit_price = static_cast<double>(biz.unit_price_cents) / 100.0;
    rec.cumulative_revenue_cents = biz.revenue_cents;
    store.upsert_business(rec);
  }
}

void EconomySystem::load_from_persistent_store(
    const persistent_world::PersistentWorldStore& store) {
  for (auto& hh : households_) {
    if (const auto* rec = store.find_npc(hh.npc_id)) {
      hh.balance_cents = rec->balance_cents;
    }
  }
  for (auto& biz : businesses_) {
    if (const auto* rec = store.find_business(biz.venue_id)) {
      biz.inventory_units = rec->inventory_units;
      biz.revenue_cents = rec->cumulative_revenue_cents;
    }
  }
}

std::int64_t EconomySystem::total_money_cents() const noexcept {
  std::int64_t sum = supplier_treasury_cents_;
  for (const auto& h : households_) sum += h.balance_cents;
  for (const auto& b : businesses_) sum += b.cash_cents;
  return sum;
}

}  // namespace saige::economy
