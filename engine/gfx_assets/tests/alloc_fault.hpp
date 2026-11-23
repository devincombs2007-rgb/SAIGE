// Allocation-failure injection for tests: makes the n-th global operator new after a call throw std::bad_alloc, so a test can sweep every
// allocation point of a function and check that it leaves its arguments untouched when one fails. The replacement operators live in
// alloc_fault.cpp (one translation unit per test executable). Not available with MSVC's debug STL, whose checked containers allocate in
// places a sweep would break (available() says so, and sweep tests skip).
#pragma once

#include <cstdint>

namespace saige::test {

/// True where fault injection is compiled in.
[[nodiscard]] bool alloc_fault_available() noexcept;

/// The n-th (1-based) global operator new call after this one throws std::bad_alloc; 0 cancels. Consumed by the allocation it fails.
void fail_allocation_after(uint32_t n) noexcept;

/// Allocations since the last reset (for sizing a sweep).
[[nodiscard]] uint64_t allocations_seen() noexcept;

} // namespace saige::test
