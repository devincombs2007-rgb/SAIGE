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
    /// `retireAfter`: the item may be released once the completed timeline value >= retireAfter.
    void enqueue(T item, uint64_t retireAfter) { m_entries.push_back({std::move(item), retireAfter}); }

    /// Calls `onRelease(T&&)` for every item whose retire value <= `completedValue`, in enqueue order.
    /// Returns the number of items released.
    template <typename Fn>
    std::size_t collect(uint64_t completedValue, Fn&& onRelease)
    {
        std::size_t kept = 0;
        std::size_t released = 0;
        for (std::size_t i = 0; i < m_entries.size(); ++i) {
            if (m_entries[i].retireAfter <= completedValue) {
                onRelease(std::move(m_entries[i].item));
                ++released;
            } else {
                if (kept != i) {
                    m_entries[kept] = std::move(m_entries[i]);
                }
                ++kept;
            }
        }
        m_entries.resize(kept);
        return released;
    }

    /// Releases everything regardless of timeline value (device shutdown after a wait-idle).
    template <typename Fn>
    std::size_t drainAll(Fn&& onRelease)
    {
        const std::size_t n = m_entries.size();
        for (Entry& e : m_entries) {
            onRelease(std::move(e.item));
        }
        m_entries.clear();
        return n;
    }

    [[nodiscard]] std::size_t size() const noexcept { return m_entries.size(); }
    [[nodiscard]] bool empty() const noexcept { return m_entries.empty(); }

private:
    struct Entry {
        T item;
        uint64_t retireAfter;
    };
    std::vector<Entry> m_entries;
};

} // namespace saige::rhi
