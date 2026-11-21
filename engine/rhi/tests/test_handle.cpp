#include "saige/gfx_test/saige_test.h"

#include "saige/rhi/handle.h"
#include "saige/rhi/handle_pool.h"

#include <memory>
#include <string>
#include <type_traits>
#include <unordered_set>

using namespace saige::rhi;

namespace {
struct TestTag;
}

// Compile-time properties
static_assert(std::is_trivially_copyable_v<TextureHandle>);
static_assert(!std::is_convertible_v<TextureHandle, BufferHandle>, "Handle types must not interconvert");
static_assert(!TextureHandle{}.isValid());
static_assert(TextureHandle::fromParts(5, 3).index() == 5);
static_assert(TextureHandle::fromParts(5, 3).generation() == 3);
static_assert(TextureHandle::nextGeneration(TextureHandle::kMaxGeneration) == 1, "Generation must skip 0 on wrap");

TEST_CASE("Handle: packs index and generation, round-trips through raw")
{
    const auto h = BufferHandle::fromParts(BufferHandle::kMaxIndex, BufferHandle::kMaxGeneration);
    CHECK(h.index() == BufferHandle::kMaxIndex);
    CHECK(h.generation() == BufferHandle::kMaxGeneration);
    CHECK(BufferHandle::fromRaw(h.raw()) == h);
    CHECK(h.isValid());
    CHECK(static_cast<bool>(h));
}

TEST_CASE("Handle: hashable and usable in unordered containers")
{
    std::unordered_set<SamplerHandle> set;
    set.insert(SamplerHandle::fromParts(1, 1));
    set.insert(SamplerHandle::fromParts(1, 2));
    set.insert(SamplerHandle::fromParts(1, 1));
    CHECK(set.size() == 2);
}

TEST_CASE("HandlePool: emplace, get and release")
{
    HandlePool<std::string, TestTag> pool;
    const auto a = pool.emplace("alpha");
    const auto b = pool.emplace("beta");
    REQUIRE(a.isValid());
    REQUIRE(b.isValid());
    CHECK(a != b);
    CHECK(pool.size() == 2);

    REQUIRE(pool.get(a) != nullptr);
    CHECK(*pool.get(a) == "alpha");
    CHECK(*pool.get(b) == "beta");

    auto released = pool.release(a);
    REQUIRE(released.has_value());
    CHECK(*released == "alpha");
    CHECK(pool.size() == 1);
    CHECK(pool.get(a) == nullptr);
    CHECK_FALSE(pool.contains(a));
}

TEST_CASE("HandlePool: stale handles are rejected after slot reuse")
{
    HandlePool<int, TestTag> pool;
    const auto first = pool.emplace(1);
    pool.release(first);
    const auto second = pool.emplace(2);

    CHECK(second.index() == first.index());  // slot reused...
    CHECK(second.generation() != first.generation());  // ...with a new generation
    CHECK(pool.get(first) == nullptr);
    CHECK_FALSE(pool.release(first).has_value());  // double release / stale release is a no-op
    REQUIRE(pool.get(second) != nullptr);
    CHECK(*pool.get(second) == 2);
}

TEST_CASE("HandlePool: invalid and out-of-range handles are rejected")
{
    HandlePool<int, TestTag> pool;
    CHECK(pool.get({}) == nullptr);
    CHECK(pool.get(Handle<TestTag>::fromParts(1000, 1)) == nullptr);
    CHECK_FALSE(pool.release({}).has_value());
}

TEST_CASE("HandlePool: freed slots are reused FIFO")
{
    HandlePool<int, TestTag> pool;
    const auto a = pool.emplace(0);
    const auto b = pool.emplace(1);
    const auto c = pool.emplace(2);
    pool.release(b);
    pool.release(a);
    CHECK(pool.emplace(3).index() == b.index());
    CHECK(pool.emplace(4).index() == a.index());
    CHECK(pool.emplace(5).index() == c.index() + 1);
    CHECK(pool.slotCount() == 4);
}

TEST_CASE("HandlePool: respects maxSlots")
{
    HandlePool<int, TestTag> pool(2);
    CHECK(pool.emplace(0).isValid());
    CHECK(pool.emplace(1).isValid());
    CHECK_FALSE(pool.emplace(2).isValid());
}

TEST_CASE("HandlePool: clear invalidates all handles and supports move-only types")
{
    HandlePool<std::unique_ptr<int>, TestTag> pool;
    const auto a = pool.emplace(std::make_unique<int>(7));
    const auto b = pool.emplace(std::make_unique<int>(8));
    pool.clear();
    CHECK(pool.empty());
    CHECK(pool.get(a) == nullptr);
    CHECK(pool.get(b) == nullptr);

    const auto c = pool.emplace(std::make_unique<int>(9));
    REQUIRE(pool.get(c) != nullptr);
    CHECK(**pool.get(c) == 9);
}

TEST_CASE("HandlePool: forEach visits live objects with valid handles")
{
    HandlePool<int, TestTag> pool;
    const auto a = pool.emplace(10);
    const auto b = pool.emplace(20);
    const auto c = pool.emplace(30);
    pool.release(b);

    int sum = 0;
    int visits = 0;
    pool.forEach([&](auto handle, int& value) {
        CHECK(pool.contains(handle));
        sum += value;
        ++visits;
    });
    CHECK(visits == 2);
    CHECK(sum == 40);
    (void)a;
    (void)c;
}

TEST_CASE("HandlePool: generation wraps without producing generation 0")
{
    HandlePool<int, TestTag> pool;
    auto h = pool.emplace(0);
    for (uint32_t i = 0; i < Handle<TestTag>::kMaxGeneration + 5; ++i) {
        pool.release(h);
        h = pool.emplace(static_cast<int>(i));
        REQUIRE(h.isValid());
        REQUIRE(h.generation() != 0);
    }
    CHECK(pool.slotCount() == 1);
}
