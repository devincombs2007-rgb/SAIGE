#include "saige/gfx_test/saige_test.hpp"

#include "saige/rhi/bindless_allocator.hpp"
#include "saige/rhi/deferred_release_queue.hpp"

#include <string>
#include <vector>

using namespace saige::rhi;

TEST_CASE("DeferredReleaseQueue: releases only items whose timeline value completed")
{
    DeferredReleaseQueue<std::string> q;
    q.enqueue("frame1-a", 1);
    q.enqueue("frame2", 2);
    q.enqueue("frame1-b", 1);
    q.enqueue("frame3", 3);

    std::vector<std::string> released;
    auto sink = [&](std::string&& s) { released.push_back(std::move(s)); };

    CHECK(q.collect(0, sink) == 0);
    CHECK(q.collect(1, sink) == 2);
    REQUIRE(released.size() == 2);
    CHECK(released[0] == "frame1-a");  // enqueue order preserved
    CHECK(released[1] == "frame1-b");
    CHECK(q.size() == 2);

    CHECK(q.collect(5, sink) == 2);
    CHECK(q.empty());
}

TEST_CASE("DeferredReleaseQueue: drain_all releases everything")
{
    DeferredReleaseQueue<int> q;
    q.enqueue(1, 100);
    q.enqueue(2, 200);
    int sum = 0;
    CHECK(q.drain_all([&](int&& v) { sum += v; }) == 2);
    CHECK(sum == 3);
    CHECK(q.empty());
}

TEST_CASE("BindlessIndexAllocator: reserved indices are never handed out")
{
    BindlessIndexAllocator alloc(8, 2);
    CHECK(alloc.allocate() == 2);
    CHECK(alloc.allocate() == 3);
    CHECK_FALSE(alloc.is_allocated(0));
    CHECK_FALSE(alloc.release(0, 0));  // cannot release reserved
    CHECK_FALSE(alloc.release(1, 0));
}

TEST_CASE("BindlessIndexAllocator: exhaustion returns invalid_index")
{
    BindlessIndexAllocator alloc(4, 1);
    CHECK(alloc.allocate() == 1);
    CHECK(alloc.allocate() == 2);
    CHECK(alloc.allocate() == 3);
    CHECK(alloc.allocate() == BindlessIndexAllocator::invalid_index);
    CHECK(alloc.allocated_count() == 3);
}

TEST_CASE("BindlessIndexAllocator: released indices wait for the GPU timeline")
{
    BindlessIndexAllocator alloc(3, 1);
    const uint32_t a = alloc.allocate();
    const uint32_t b = alloc.allocate();
    REQUIRE(alloc.allocate() == BindlessIndexAllocator::invalid_index);

    // Destroyed in frame 10: must not be reused before the GPU finishes frame 10.
    CHECK(alloc.release(a, 10));
    CHECK_FALSE(alloc.is_allocated(a));
    CHECK(alloc.pending_count() == 1);
    CHECK(alloc.allocate() == BindlessIndexAllocator::invalid_index);

    CHECK(alloc.reclaim(9) == 0);
    CHECK(alloc.allocate() == BindlessIndexAllocator::invalid_index);

    CHECK(alloc.reclaim(10) == 1);
    CHECK(alloc.allocate() == a);
    (void)b;
}

TEST_CASE("BindlessIndexAllocator: double release and stale release are rejected")
{
    BindlessIndexAllocator alloc(8, 1);
    const uint32_t a = alloc.allocate();
    CHECK(alloc.release(a, 1));
    CHECK_FALSE(alloc.release(a, 2));  // pending
    alloc.reclaim(1);
    CHECK_FALSE(alloc.release(a, 3));  // free
    CHECK_FALSE(alloc.release(100, 3));  // out of range
    CHECK_FALSE(alloc.release(BindlessIndexAllocator::invalid_index, 3));
}

TEST_CASE("BindlessIndexAllocator: recycled indices are preferred, keeping the high-water mark low")
{
    BindlessIndexAllocator alloc(1000, 1);
    std::vector<uint32_t> live;
    for (int i = 0; i < 10; ++i) {
        live.push_back(alloc.allocate());
    }
    CHECK(alloc.high_water_mark() == 11);

    for (uint32_t idx : live) {
        alloc.release(idx, 1);
    }
    alloc.reclaim(1);
    for (int i = 0; i < 10; ++i) {
        const uint32_t idx = alloc.allocate();
        CHECK(idx >= 1);
        CHECK(idx <= 10);
    }
    CHECK(alloc.high_water_mark() == 11);
    CHECK(alloc.allocated_count() == 10);
}
