// SAIGE RHI — queue of objects whose release must wait for the GPU.
#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace saige::rhi {

/// Holds items until a GPU timeline value has been reached.
///
/// Typical use: when a resource is destroyed during frame N, enqueue it with that frame's timeline
/// signal value. Once the GPU reports completion of that value (timeline semaphore), `collect()` hands
/// the item back for real destruction. The RHI uses this for GPU objects, memory allocations and
/// bindless descriptor indices.
///
/// Not thread-safe.
template <typename T>
class DeferredReleaseQueue {
public:
    /// `retire_after`: the item may be released once the completed timeline value >= retire_after.
    void enqueue(T item, uint64_t retire_after) { entries_.push_back({std::move(item), retire_after}); }

    /// Calls `on_release(T&&)` for every item whose retire value <= `completed_value`, in enqueue order.
    /// Returns the number of items released.
    template <typename Fn>
    std::size_t collect(uint64_t completed_value, Fn&& on_release)
    {
        std::size_t kept = 0;
        std::size_t released = 0;
        for (std::size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].retire_after <= completed_value) {
                on_release(std::move(entries_[i].item));
                ++released;
            } else {
                if (kept != i) {
                    entries_[kept] = std::move(entries_[i]);
                }
                ++kept;
            }
        }
        entries_.resize(kept);
        return released;
    }

    /// Releases everything regardless of timeline value (device shutdown after a wait-idle).
    template <typename Fn>
    std::size_t drain_all(Fn&& on_release)
    {
        const std::size_t n = entries_.size();
        for (Entry& e : entries_) {
            on_release(std::move(e.item));
        }
        entries_.clear();
        return n;
    }

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }

private:
    struct Entry {
        T item;
        uint64_t retire_after;
    };
    std::vector<Entry> entries_;
};

} // namespace saige::rhi
