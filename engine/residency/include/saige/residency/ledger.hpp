// Residency accounting for streamed resources.
//
// Each resource has quality levels, best first: level 0 is the full texture (or the finest LOD) and each later level is smaller; `level_bytes[l]` is what level `l` costs when resident. The ledger tracks the
// level each resource currently has resident (or none). Every frame the renderer reports what it wants: for each visible resource a desired level and an importance (for example its screen coverage); `plan()`
// returns the changes that fit the budgets, `apply()` commits them. The planner works in this order, which is also its priority:
//   1. resources not requested this frame are evicted, least recently used first, when the budget is exceeded (pinned resources never are);
//   2. requested resources are degraded one level at a time, the least important first (ties by the larger saving, then by id), but never below their coarsest level while that fits;
//   3. if even the coarsest levels do not fit, whole requested resources are evicted, least important first;
//   4. what is left of the desired upgrades is limited by the per-frame upload allowance, most important first.
// Pinned resources are never evicted or degraded below their pinned level; if the pinned bytes alone exceed a budget, the plan says so (`pinned_over_budget`) instead of breaking the pin. Everything is deterministic.
// This module only counts. It neither knows about a device nor measures real memory: the renderer maps the numbers onto its own allocations and the device's reported budget.
#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace saige::residency {

enum class Category { Texture, Geometry, Other };
inline constexpr int kCategoryCount = 3;

struct Budgets {
    std::uint64_t total_bytes = UINT64_MAX;                                                 ///< unlimited by default, like the other limits
    std::uint64_t category_bytes[kCategoryCount] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};  ///< per category; unlimited by default
    std::uint64_t upload_bytes_per_frame = UINT64_MAX;                                    ///< bytes of upgrades allowed per plan
};

inline constexpr int kNotResident = -1;

struct ResourceInfo {
    Category category = Category::Texture;
    std::vector<std::uint64_t> level_bytes;  ///< non-empty, strictly decreasing, all > 0
};

struct Request {
    std::uint64_t id = 0;
    int desired_level = 0;  ///< 0 is the best level
    double importance = 1.0;  ///< >= 0
};

struct Change {
    std::uint64_t id = 0;
    int from_level = kNotResident;
    int to_level = kNotResident;
};

struct Plan {
    std::vector<Change> changes;           ///< in the order to carry them out: evictions and degrades first, then uploads
    std::uint64_t resident_bytes = 0;      ///< total after the plan
    std::uint64_t upload_bytes = 0;        ///< bytes the plan's upgrades add
    bool pinned_over_budget = false;       ///< the pinned resources alone exceed a budget
    bool upgrades_deferred = false;        ///< some desired upgrades were left for later frames (the upload allowance or the budget)
};

class Ledger {
public:
    explicit Ledger(const Budgets& budgets);
    /// Register a resource (not resident). Throws std::invalid_argument for a duplicate id or invalid levels.
    void add(std::uint64_t id, const ResourceInfo& info);
    void remove(std::uint64_t id);
    /// Pin a resource at `level` (or better): it is never evicted and never degraded below `level`. Throws for an unknown id or level. `unpin` releases it.
    void pin(std::uint64_t id, int level);
    void unpin(std::uint64_t id);
    void set_budgets(const Budgets& budgets);

    /// The plan for `frame` (increasing) given the requests. Does not change the ledger. Throws std::invalid_argument for an unknown id, an invalid level or a negative importance.
    [[nodiscard]] Plan plan(std::uint64_t frame, const std::vector<Request>& requests) const;
    /// Commit a plan produced for the same ledger state and frame (the frame stamps the recency of the requested resources).
    void apply(std::uint64_t frame, const std::vector<Request>& requests, const Plan& plan);

    [[nodiscard]] int resident_level(std::uint64_t id) const;
    [[nodiscard]] std::uint64_t resident_bytes() const;
    [[nodiscard]] std::uint64_t resident_bytes(Category category) const;
    [[nodiscard]] std::size_t resource_count() const noexcept { return resources_.size(); }

private:
    struct Entry {
        ResourceInfo info;
        int level = kNotResident;
        int pinned_level = kNotResident;  ///< kNotResident when not pinned
        std::uint64_t last_used = 0;
    };
    [[nodiscard]] static std::uint64_t bytes_at(const Entry& e, int level);
    std::map<std::uint64_t, Entry> resources_;
    Budgets budgets_;
};

} // namespace saige::residency
