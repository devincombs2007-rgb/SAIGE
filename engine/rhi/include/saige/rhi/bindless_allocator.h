// SAIGE RHI — allocator for slots in a global bindless descriptor array.
#pragma once

#include "saige/rhi/deferred_release_queue.h"

#include <cstdint>
#include <deque>
#include <vector>

namespace saige::rhi {

/// Hands out stable indices into one bindless descriptor array (e.g., all sampled textures).
///
/// - Indices `[0, reservedCount)` are reserved for fallback "error" resources (index 0 = magenta
///   texture / zero buffer), so a zero-initialized index in shader data is always safe to sample.
/// - Released indices go back to the free list only after the GPU timeline passes the given value,
///   so in-flight frames never see a descriptor overwritten under them.
/// - Freed indices are reused FIFO and preferred over never-used ones, which keeps the descriptor
///   high-water mark (and the update-after-bind range) compact.
/// - Not thread-safe.
class BindlessIndexAllocator {
public:
    static constexpr uint32_t kInvalidIndex = 0xFFFFFFFFu;

    explicit BindlessIndexAllocator(uint32_t capacity, uint32_t reservedCount = 1);

    /// Returns a free index, or `kInvalidIndex` if the array is exhausted.
    [[nodiscard]] uint32_t allocate();

    /// Schedules `index` for reuse once the GPU has completed `retireAfter`.
    /// Returns false (and changes nothing) for reserved, out-of-range, free or already-released indices.
    bool release(uint32_t index, uint64_t retireAfter);

    /// Returns indices whose retire value <= `completedValue` to the free list. Returns how many.
    uint32_t reclaim(uint64_t completedValue);

    [[nodiscard]] bool isAllocated(uint32_t index) const noexcept;

    [[nodiscard]] uint32_t capacity() const noexcept { return m_capacity; }
    [[nodiscard]] uint32_t reservedCount() const noexcept { return m_reservedCount; }
    /// Live allocations (excludes reserved and pending-release indices).
    [[nodiscard]] uint32_t allocatedCount() const noexcept { return m_allocatedCount; }
    [[nodiscard]] uint32_t pendingCount() const noexcept { return static_cast<uint32_t>(m_pending.size()); }
    /// One past the highest index ever handed out (descriptor update range).
    [[nodiscard]] uint32_t highWaterMark() const noexcept { return m_nextUnused; }

private:
    enum class State : uint8_t { Free, Allocated, PendingRelease, Reserved };

    uint32_t m_capacity;
    uint32_t m_reservedCount;
    uint32_t m_nextUnused;  // indices >= this have never been handed out
    uint32_t m_allocatedCount = 0;
    std::vector<State> m_states;
    std::deque<uint32_t> m_freeList;
    DeferredReleaseQueue<uint32_t> m_pending;
};

} // namespace saige::rhi
