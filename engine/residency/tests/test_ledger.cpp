// The residency ledger: budgets are never exceeded, pins hold, importance and recency decide who loses quality, the upload allowance paces upgrades, and the planner is deterministic.
#include "saige/gfx_test/saige_test.hpp"

#include "saige/residency/ledger.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <vector>

using namespace saige::residency;

namespace {

template <class F>
bool throws(F&& f)
{
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

constexpr std::uint64_t MiB = 1024ull * 1024ull;

/// A texture with `mips` levels, level 0 costing `top` bytes and each next a quarter of the last.
ResourceInfo texture(std::uint64_t top, int mips = 5)
{
    ResourceInfo r;
    r.category = Category::Texture;
    for (int i = 0; i < mips; ++i) r.level_bytes.push_back(top >> (2 * i));
    return r;
}
ResourceInfo mesh(std::uint64_t top)
{
    ResourceInfo r;
    r.category = Category::Geometry;
    r.level_bytes = {top, top / 2, top / 8};
    return r;
}

std::uint32_t hash32(std::uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

void run(Ledger& l, std::uint64_t frame, const std::vector<Request>& r, Plan* out = nullptr)
{
    const Plan p = l.plan(frame, r);
    l.apply(frame, r, p);
    if (out != nullptr) *out = p;
}

} // namespace

TEST_CASE("residency: everything wanted is loaded when it fits, and nothing changes the next frame") {
    Budgets b;
    b.total_bytes = 100 * MiB;
    Ledger l(b);
    l.add(1, texture(16 * MiB));
    l.add(2, texture(16 * MiB));
    l.add(3, mesh(8 * MiB));
    Plan p;
    run(l, 1, {{1, 0, 1.0}, {2, 1, 1.0}, {3, 0, 1.0}}, &p);
    CHECK(l.resident_level(1) == 0);
    CHECK(l.resident_level(2) == 1);
    CHECK(l.resident_level(3) == 0);
    CHECK(p.changes.size() == 3);
    CHECK_FALSE(p.upgrades_deferred);
    CHECK_FALSE(p.pinned_over_budget);
    CHECK(l.resident_bytes() == 16 * MiB + 4 * MiB + 8 * MiB);
    run(l, 2, {{1, 0, 1.0}, {2, 1, 1.0}, {3, 0, 1.0}}, &p);
    CHECK(p.changes.empty());
    // A resident level that is already better than the one asked for is kept: no thrash.
    run(l, 3, {{1, 2, 1.0}}, &p);
    CHECK(l.resident_level(1) == 0);
    CHECK(p.changes.empty());
}

TEST_CASE("residency: the budgets are never exceeded, under random demand, per category and in total") {
    Budgets b;
    b.total_bytes = 60 * MiB;
    b.category_bytes[static_cast<int>(Category::Texture)] = 40 * MiB;
    b.category_bytes[static_cast<int>(Category::Geometry)] = 25 * MiB;
    Ledger l(b);
    for (std::uint64_t id = 1; id <= 30; ++id) l.add(id, id % 3 == 0 ? mesh((2 + id % 5) * MiB) : texture((4 + id % 4 * 4) * MiB));
    for (std::uint64_t frame = 1; frame <= 400; ++frame) {
        std::vector<Request> req;
        for (std::uint64_t id = 1; id <= 30; ++id) {
            const std::uint32_t h = hash32(static_cast<std::uint32_t>(frame * 131 + id));
            if (h % 3 == 0) continue;  // not visible this frame
            const int levels = id % 3 == 0 ? 3 : 5;
            req.push_back({id, static_cast<int>((h >> 4) % static_cast<std::uint32_t>(levels)), static_cast<double>((h >> 9) % 100) / 10.0});
        }
        run(l, frame, req);
        REQUIRE(l.resident_bytes() <= b.total_bytes);
        REQUIRE(l.resident_bytes(Category::Texture) <= 40 * MiB);
        REQUIRE(l.resident_bytes(Category::Geometry) <= 25 * MiB);
    }
}

TEST_CASE("residency: pinned resources are never evicted or degraded below their pin, and an impossible pin is reported") {
    Budgets b;
    b.total_bytes = 30 * MiB;
    Ledger l(b);
    l.add(1, texture(16 * MiB));
    l.add(2, texture(16 * MiB));
    l.add(3, texture(16 * MiB));
    l.pin(1, 1);
    run(l, 1, {{1, 3, 0.0}, {2, 0, 5.0}, {3, 0, 5.0}});  // the pin wins over the asked-for level 3, and its zero importance does not matter
    CHECK(l.resident_level(1) >= 0);
    CHECK(l.resident_level(1) <= 1);
    for (std::uint64_t f = 2; f < 40; ++f) {
        run(l, f, {{2, 0, 9.0}, {3, 0, 9.0}});  // 1 is not requested, so it is the obvious eviction candidate, but it is pinned
        REQUIRE(l.resident_level(1) >= 0);
        REQUIRE(l.resident_level(1) <= 1);
        REQUIRE(l.resident_bytes() <= b.total_bytes);
    }
    Budgets tiny;
    tiny.total_bytes = 1 * MiB;
    l.set_budgets(tiny);
    const Plan p = l.plan(50, {{1, 0, 1.0}});
    CHECK(p.pinned_over_budget);
    CHECK(l.resident_level(1) <= 1);
    l.unpin(1);
    run(l, 51, {});
    CHECK(l.resident_level(1) == kNotResident);  // unpinned and unrequested: now it goes
}

TEST_CASE("residency: under pressure the less important resource loses quality first, and the order is stable") {
    Budgets b;
    b.total_bytes = 20 * MiB;
    Ledger l(b);
    l.add(1, texture(16 * MiB));
    l.add(2, texture(16 * MiB));
    l.add(3, texture(16 * MiB));
    run(l, 1, {{1, 0, 9.0}, {2, 0, 5.0}, {3, 0, 1.0}});
    CHECK(l.resident_bytes() <= b.total_bytes);
    CHECK(l.resident_level(1) <= l.resident_level(2));
    CHECK(l.resident_level(2) <= l.resident_level(3));
    CHECK(l.resident_level(1) == 0);  // the most important one keeps what it asked for
    CHECK(l.resident_level(3) > 0);
    // Determinism: a second ledger given the same history makes the same plan.
    Ledger m(b);
    m.add(1, texture(16 * MiB));
    m.add(2, texture(16 * MiB));
    m.add(3, texture(16 * MiB));
    const Plan a = m.plan(1, {{1, 0, 9.0}, {2, 0, 5.0}, {3, 0, 1.0}});
    const Plan c = Ledger(m).plan(1, {{3, 0, 1.0}, {1, 0, 9.0}, {2, 0, 5.0}});  // the request order does not matter
    REQUIRE(a.changes.size() == c.changes.size());
    for (std::size_t i = 0; i < a.changes.size(); ++i) {
        CHECK(a.changes[i].id == c.changes[i].id);
        CHECK(a.changes[i].to_level == c.changes[i].to_level);
    }
}

TEST_CASE("residency: unrequested resources go in least-recently-used order before anything requested loses quality") {
    Budgets b;
    b.total_bytes = 52 * MiB;  // three full 16 MiB textures fit, four do not
    Ledger l(b);
    for (std::uint64_t id = 1; id <= 4; ++id) l.add(id, texture(16 * MiB, 3));
    run(l, 1, {{1, 0, 1.0}});
    run(l, 2, {{2, 0, 1.0}});
    run(l, 3, {{3, 0, 1.0}});
    run(l, 4, {{1, 0, 1.0}});  // 1 is used again: the recency order is now 2 (frame 2), 3 (frame 3), 1 (frame 4)
    CHECK(l.resident_level(1) == 0);
    CHECK(l.resident_level(2) == 0);
    CHECK(l.resident_level(3) == 0);
    Plan p;
    run(l, 5, {{4, 0, 1.0}}, &p);  // 4 needs room: only one eviction is needed, and it must be the least recently used, 2
    CHECK(l.resident_level(2) == kNotResident);
    CHECK(l.resident_level(1) == 0);
    CHECK(l.resident_level(3) == 0);
    CHECK(l.resident_level(4) == 0);  // the requested one kept the quality it asked for
    CHECK(l.resident_bytes() <= b.total_bytes);
    CHECK_FALSE(p.upgrades_deferred);
    run(l, 6, {{2, 0, 1.0}}, &p);  // asking for 2 again evicts the next oldest: 3 (frame 3), not 1 (frame 4) or 4 (frame 5)
    CHECK(l.resident_level(3) == kNotResident);
    CHECK(l.resident_level(1) == 0);
    CHECK(l.resident_level(4) == 0);
}

TEST_CASE("residency: the upload allowance paces upgrades, most important first, and later frames finish the job") {
    Budgets b;
    b.total_bytes = 200 * MiB;
    b.upload_bytes_per_frame = 20 * MiB;
    Ledger l(b);
    for (std::uint64_t id = 1; id <= 4; ++id) l.add(id, texture(16 * MiB));
    std::vector<Request> req{{1, 0, 1.0}, {2, 0, 4.0}, {3, 0, 3.0}, {4, 0, 2.0}};
    Plan p;
    run(l, 1, req, &p);
    CHECK(p.upload_bytes <= 20 * MiB);
    CHECK(p.upgrades_deferred);
    CHECK(l.resident_level(2) == 0);  // the most important got its full texture first
    for (std::uint64_t f = 2; f < 10; ++f) {
        run(l, f, req, &p);
        REQUIRE(p.upload_bytes <= 20 * MiB);
    }
    for (std::uint64_t id = 1; id <= 4; ++id) CHECK(l.resident_level(id) == 0);
    CHECK_FALSE(p.upgrades_deferred);
    // A partial upgrade: only a coarser level fits the allowance.
    Budgets tight;
    tight.total_bytes = 200 * MiB;
    tight.upload_bytes_per_frame = 6 * MiB;
    Ledger m(tight);
    m.add(1, texture(16 * MiB));
    run(m, 1, {{1, 0, 1.0}}, &p);
    CHECK(m.resident_level(1) == 1);  // 4 MiB fits in 6 MiB, 16 MiB does not
    run(m, 2, {{1, 0, 1.0}}, &p);     // upgrading 1 -> 0 costs 12 MiB: does not fit; 1 stays
    CHECK(m.resident_level(1) == 1);
    CHECK(p.upgrades_deferred);
}

TEST_CASE("residency: requested resources keep their coarsest level while that fits, and are evicted only when it cannot") {
    Budgets b;
    b.total_bytes = 8 * MiB;
    Ledger l(b);
    for (std::uint64_t id = 1; id <= 4; ++id) l.add(id, texture(16 * MiB));  // coarsest level 4 is 64 KiB
    run(l, 1, {{1, 0, 1.0}, {2, 0, 1.0}, {3, 0, 1.0}, {4, 0, 1.0}});
    for (std::uint64_t id = 1; id <= 4; ++id) CHECK(l.resident_level(id) != kNotResident);
    CHECK(l.resident_bytes() <= b.total_bytes);
    Budgets starve;
    starve.total_bytes = 100 * 1024;  // two coarsest levels (64 KiB each) do not fit
    l.set_budgets(starve);
    run(l, 2, {{1, 0, 5.0}, {2, 0, 1.0}, {3, 0, 2.0}, {4, 0, 3.0}});
    CHECK(l.resident_bytes() <= starve.total_bytes);
    CHECK(l.resident_level(1) != kNotResident);  // the most important survives
    CHECK(l.resident_level(2) == kNotResident);  // the least important is gone
}

TEST_CASE("residency: invalid use is rejected") {
    Ledger l(Budgets{});
    l.add(1, texture(MiB));
    CHECK(throws([&] { l.add(1, texture(MiB)); }));
    CHECK(throws([&] { l.add(2, ResourceInfo{}); }));
    ResourceInfo bad;
    bad.level_bytes = {10, 10};
    CHECK(throws([&] { l.add(3, bad); }));
    CHECK(throws([&] { (void)l.plan(1, {{9, 0, 1.0}}); }));
    CHECK(throws([&] { (void)l.plan(1, {{1, 7, 1.0}}); }));
    CHECK(throws([&] { (void)l.plan(1, {{1, 0, -1.0}}); }));
    CHECK(throws([&] { (void)l.plan(1, {{1, 0, 1.0}, {1, 0, 1.0}}); }));
    CHECK(throws([&] { l.pin(1, 9); }));
    CHECK(throws([&] { l.remove(77); }));
    CHECK(throws([&] { (void)l.resident_level(77); }));
    const Plan p = l.plan(1, {{1, 0, 1.0}});
    Ledger other(Budgets{});
    other.add(1, texture(MiB));
    run(other, 1, {{1, 3, 1.0}});
    CHECK(throws([&] { other.apply(2, {}, p); }));  // made for a different state
}
