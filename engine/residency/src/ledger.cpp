#include "saige/residency/ledger.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::residency {

namespace {

constexpr std::size_t cat_index(Category c) { return static_cast<std::size_t>(c); }

} // namespace

std::uint64_t Ledger::bytes_at(const Entry& e, int level)
{
    return level < 0 ? 0 : e.info.level_bytes[static_cast<std::size_t>(level)];
}

Ledger::Ledger(const Budgets& budgets) : budgets_(budgets) {}

void Ledger::set_budgets(const Budgets& budgets) { budgets_ = budgets; }

void Ledger::add(std::uint64_t id, const ResourceInfo& info)
{
    if (resources_.count(id) != 0) throw std::invalid_argument("Ledger::add: the id is already registered");
    if (info.level_bytes.empty()) throw std::invalid_argument("Ledger::add: a resource needs at least one level");
    for (std::size_t i = 0; i < info.level_bytes.size(); ++i) {
        if (info.level_bytes[i] == 0 || (i > 0 && info.level_bytes[i] >= info.level_bytes[i - 1])) throw std::invalid_argument("Ledger::add: the level sizes must be positive and strictly decreasing");
    }
    resources_[id] = Entry{info, kNotResident, kNotResident, 0};
}

void Ledger::remove(std::uint64_t id)
{
    if (resources_.erase(id) == 0) throw std::invalid_argument("Ledger::remove: unknown id");
}

void Ledger::pin(std::uint64_t id, int level)
{
    const auto it = resources_.find(id);
    if (it == resources_.end()) throw std::invalid_argument("Ledger::pin: unknown id");
    if (level < 0 || static_cast<std::size_t>(level) >= it->second.info.level_bytes.size()) throw std::invalid_argument("Ledger::pin: invalid level");
    it->second.pinned_level = level;
}

void Ledger::unpin(std::uint64_t id)
{
    const auto it = resources_.find(id);
    if (it == resources_.end()) throw std::invalid_argument("Ledger::unpin: unknown id");
    it->second.pinned_level = kNotResident;
}

int Ledger::resident_level(std::uint64_t id) const
{
    const auto it = resources_.find(id);
    if (it == resources_.end()) throw std::invalid_argument("Ledger::resident_level: unknown id");
    return it->second.level;
}

std::uint64_t Ledger::resident_bytes() const
{
    std::uint64_t sum = 0;
    for (const auto& kv : resources_) sum += bytes_at(kv.second, kv.second.level);
    return sum;
}

std::uint64_t Ledger::resident_bytes(Category category) const
{
    std::uint64_t sum = 0;
    for (const auto& kv : resources_)
        if (kv.second.info.category == category) sum += bytes_at(kv.second, kv.second.level);
    return sum;
}

Plan Ledger::plan(std::uint64_t frame, const std::vector<Request>& requests) const
{
    (void)frame;
    struct Work {
        const Entry* entry = nullptr;
        int current = kNotResident;
        int state = kNotResident;  ///< the level the plan settles on before the upload limit
        bool requested = false;
        double importance = 0.0;
        int floor = kNotResident;  ///< the coarsest level a requested resource may be degraded to
    };
    std::map<std::uint64_t, Work> w;
    for (const auto& kv : resources_) {
        Work x;
        x.entry = &kv.second;
        x.current = x.state = kv.second.level;
        w[kv.first] = x;
    }
    for (const Request& r : requests) {
        const auto it = w.find(r.id);
        if (it == w.end()) throw std::invalid_argument("Ledger::plan: a request names an unknown id");
        Work& x = it->second;
        if (x.requested) throw std::invalid_argument("Ledger::plan: a resource is requested twice");
        const int coarsest = static_cast<int>(x.entry->info.level_bytes.size()) - 1;
        if (r.desired_level < 0 || r.desired_level > coarsest) throw std::invalid_argument("Ledger::plan: a request's level is out of range");
        if (!std::isfinite(r.importance) || r.importance < 0.0) throw std::invalid_argument("Ledger::plan: the importance must be finite and >= 0");
        x.requested = true;
        x.importance = r.importance;
        int want = r.desired_level;
        const int pinned = x.entry->pinned_level;
        if (pinned != kNotResident) want = std::min(want, pinned);
        x.floor = pinned != kNotResident ? pinned : coarsest;
        // A resident level that is already as good as the one asked for is kept (no thrash); otherwise the plan wants the asked-for level.
        x.state = (x.current != kNotResident && x.current <= want) ? x.current : want;
    }

    Plan plan;
    auto totals = [&](std::uint64_t& total, std::uint64_t (&cat)[kCategoryCount]) {
        total = 0;
        for (auto& c : cat) c = 0;
        for (const auto& kv : w) {
            const std::uint64_t b = bytes_at(*kv.second.entry, kv.second.state);
            total += b;
            cat[cat_index(kv.second.entry->info.category)] += b;
        }
    };
    // Which budgets are exceeded: bit 0 the total, bit (1 + c) a category.
    auto violated = [&]() {
        std::uint64_t total, cat[kCategoryCount];
        totals(total, cat);
        unsigned mask = total > budgets_.total_bytes ? 1u : 0u;
        for (int c = 0; c < kCategoryCount; ++c)
            if (cat[c] > budgets_.category_bytes[c]) mask |= 1u << (1 + c);
        return mask;
    };
    auto helps = [&](unsigned mask, Category c) { return (mask & 1u) != 0 || (mask & (1u << (1 + static_cast<int>(c)))) != 0; };

    // 1. Evict resources nobody asked for, least recently used first.
    for (unsigned mask = violated(); mask != 0; mask = violated()) {
        Work* victim = nullptr;
        for (auto& kv : w) {
            Work& x = kv.second;
            if (x.requested || x.state == kNotResident || x.entry->pinned_level != kNotResident || !helps(mask, x.entry->info.category)) continue;
            if (victim == nullptr || x.entry->last_used < victim->entry->last_used) {
                victim = &x;
            }
        }
        if (victim == nullptr) break;
        victim->state = kNotResident;
    }
    // 2. Degrade requested resources one level at a time, the least important first.
    for (unsigned mask = violated(); mask != 0; mask = violated()) {
        Work* pick = nullptr;
        std::uint64_t pick_saving = 0;
        for (auto& kv : w) {
            Work& x = kv.second;
            if (!x.requested || x.state == kNotResident || x.state >= x.floor || !helps(mask, x.entry->info.category)) continue;
            const std::uint64_t saving = bytes_at(*x.entry, x.state) - bytes_at(*x.entry, x.state + 1);
            if (pick == nullptr || x.importance < pick->importance || (x.importance == pick->importance && saving > pick_saving)) {
                pick = &x;
                pick_saving = saving;
            }
        }
        if (pick == nullptr) break;
        pick->state += 1;
    }
    // 3. Even the coarsest levels do not fit: evict whole requested resources, the least important first.
    for (unsigned mask = violated(); mask != 0; mask = violated()) {
        Work* pick = nullptr;
        for (auto& kv : w) {
            Work& x = kv.second;
            if (!x.requested || x.state == kNotResident || x.entry->pinned_level != kNotResident || !helps(mask, x.entry->info.category)) continue;
            if (pick == nullptr || x.importance < pick->importance) pick = &x;
        }
        if (pick == nullptr) break;
        pick->state = kNotResident;
    }
    plan.pinned_over_budget = violated() != 0;

    // 4. The upload allowance: upgrades in order of importance, a partial upgrade if the whole one does not fit.
    std::vector<std::pair<std::uint64_t, Work*>> upgrades;
    for (auto& kv : w)
        if (kv.second.state != kNotResident && (kv.second.current == kNotResident || kv.second.state < kv.second.current)) upgrades.push_back({kv.first, &kv.second});
    std::sort(upgrades.begin(), upgrades.end(), [](const auto& a, const auto& b) { return a.second->importance != b.second->importance ? a.second->importance > b.second->importance : a.first < b.first; });
    std::uint64_t spent = 0;
    for (auto& up : upgrades) {
        Work& x = *up.second;
        const std::uint64_t have = bytes_at(*x.entry, x.current);
        const int coarsest = static_cast<int>(x.entry->info.level_bytes.size()) - 1;
        const int last_better = x.current == kNotResident ? coarsest : x.current - 1;  // the coarsest level that is still an upgrade
        int chosen = x.current;                                                       // nothing fits: the resource stays as it is
        for (int level = x.state; level <= last_better; ++level) {
            const std::uint64_t cost = bytes_at(*x.entry, level) - have;
            if (cost <= budgets_.upload_bytes_per_frame - spent) {
                spent += cost;
                chosen = level;
                break;
            }
        }
        if (chosen != x.state) plan.upgrades_deferred = true;
        x.state = chosen;
    }
    plan.upload_bytes = spent;

    // The changes: evictions and degrades first, then the uploads.
    for (const auto& kv : w)
        if (kv.second.state != kv.second.current && (kv.second.state == kNotResident || (kv.second.current != kNotResident && kv.second.state > kv.second.current))) plan.changes.push_back({kv.first, kv.second.current, kv.second.state});
    for (const auto& kv : w)
        if (kv.second.state != kv.second.current && kv.second.state != kNotResident && (kv.second.current == kNotResident || kv.second.state < kv.second.current)) plan.changes.push_back({kv.first, kv.second.current, kv.second.state});
    for (const auto& kv : w) plan.resident_bytes += bytes_at(*kv.second.entry, kv.second.state);
    return plan;
}

void Ledger::apply(std::uint64_t frame, const std::vector<Request>& requests, const Plan& plan)
{
    for (const Change& c : plan.changes) {
        const auto it = resources_.find(c.id);
        if (it == resources_.end()) throw std::invalid_argument("Ledger::apply: the plan names an unknown id");
        if (c.to_level < kNotResident || c.to_level >= static_cast<int>(it->second.info.level_bytes.size())) throw std::invalid_argument("Ledger::apply: the plan has an invalid level");
        if (it->second.level != c.from_level) throw std::invalid_argument("Ledger::apply: the plan was made for a different ledger state");
    }
    for (const Change& c : plan.changes) resources_[c.id].level = c.to_level;
    for (const Request& r : requests) {
        const auto it = resources_.find(r.id);
        if (it != resources_.end()) it->second.last_used = frame;
    }
}

} // namespace saige::residency
