// SAIGE RHI — allocator for slots in a global bindless descriptor array.
#pragma once

#include "saige/rhi/deferred_release_queue.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace saige::rhi {

/// Hands out stable indices into one bindless descriptor array (e.g., all sampled textures).
///
/// - Indices `[0, reserved_count)` are reserved for fallback "error" resources (index 0 = magenta
///   texture / zero buffer), so a zero-initialized index in shader data is always safe to sample.
/// - Released indices go back to the free list only after the GPU timeline passes the given value,
///   so in-flight frames never see a descriptor overwritten under them.
/// - Freed indices are reused FIFO and preferred over never-used ones, which keeps the descriptor
///   high-water mark (and the update-after-bind range) compact.
/// - Not thread-safe.
class BindlessIndexAllocator {
public:
    static constexpr uint32_t invalid_index = 0xFFFFFFFFu;

    explicit BindlessIndexAllocator(uint32_t capacity, uint32_t reserved_count = 1);

    /// Returns a free index, or `invalid_index` if the array is exhausted.
    [[nodiscard]] uint32_t allocate();

    /// Schedules `index` for reuse once the GPU has completed `retire_after`.
    /// Returns false (and changes nothing) for reserved, out-of-range, free or already-released indices.
    bool release(uint32_t index, uint64_t retire_after);

    /// Returns indices whose retire value <= `completed_value` to the free list. Returns how many.
    uint32_t reclaim(uint64_t completed_value);

    [[nodiscard]] bool is_allocated(uint32_t index) const noexcept;

    [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] uint32_t reserved_count() const noexcept { return reserved_count_; }
    /// Live allocations (excludes reserved and pending-release indices).
    [[nodiscard]] uint32_t allocated_count() const noexcept { return allocated_count_; }
    [[nodiscard]] uint32_t pending_count() const noexcept { return static_cast<uint32_t>(pending_.size()); }
    /// One past the highest index ever handed out (descriptor update range).
    [[nodiscard]] uint32_t high_water_mark() const noexcept { return next_unused_; }

private:
    enum class State : uint8_t { Free, Allocated, PendingRelease, Reserved };

    uint32_t capacity_;
    uint32_t reserved_count_;
    uint32_t next_unused_;  // indices >= this have never been handed out
    uint32_t allocated_count_ = 0;
    std::vector<State> states_;
    std::deque<uint32_t> free_list_;
    DeferredReleaseQueue<uint32_t> pending_;
};

} // namespace saige::rhi
