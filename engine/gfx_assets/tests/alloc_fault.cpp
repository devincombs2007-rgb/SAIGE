#include "alloc_fault.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<uint32_t> g_countdown{0};
std::atomic<uint64_t> g_seen{0};
} // namespace

namespace saige::test {

#if !defined(_MSC_VER)
bool alloc_fault_available() noexcept { return true; }
#else
bool alloc_fault_available() noexcept { return false; }
#endif

void fail_allocation_after(uint32_t n) noexcept { g_countdown.store(n, std::memory_order_relaxed); }
uint64_t allocations_seen() noexcept { return g_seen.load(std::memory_order_relaxed); }

} // namespace saige::test

#if !defined(_MSC_VER)
void* operator new(std::size_t size)
{
    g_seen.fetch_add(1, std::memory_order_relaxed);
    const uint32_t c = g_countdown.load(std::memory_order_relaxed);
    if (c != 0 && g_countdown.fetch_sub(1, std::memory_order_relaxed) == 1) {
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size != 0 ? size : 1)) {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
#endif
