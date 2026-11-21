// SAIGE RHI — generational object pool addressed by Handle<Tag>.
#pragma once

#include "saige/rhi/config.h"
#include "saige/rhi/handle.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

namespace saige::rhi {

/// Slot-based pool that stores `T` objects and hands out generational handles.
///
/// - O(1) insert, lookup and release.
/// - Stale handles (released, or from before a `clear()`) are rejected by `get`, `contains` and `release`.
/// - Freed slots are reused in FIFO order, which delays reuse of any given slot and makes
///   generation wrap-around (4095 reuses of the same slot) unlikely to alias a stale handle.
/// - Not thread-safe: callers synchronize externally (backends own one pool per object type).
template <typename T, typename Tag>
class HandlePool {
public:
    using HandleType = Handle<Tag>;

    explicit HandlePool(uint32_t maxSlots = HandleType::kMaxSlots) noexcept
        : m_maxSlots(maxSlots < HandleType::kMaxSlots ? maxSlots : HandleType::kMaxSlots)
    {
    }

    /// Constructs a `T` in a free slot. Returns an invalid handle if the pool is full.
    template <typename... Args>
    [[nodiscard]] HandleType emplace(Args&&... args)
    {
        uint32_t index = 0;
        if (!m_freeList.empty()) {
            index = m_freeList.front();
            m_freeList.pop_front();
        } else {
            if (m_slots.size() >= m_maxSlots) {
                return HandleType{};
            }
            index = static_cast<uint32_t>(m_slots.size());
            m_slots.emplace_back();
        }

        Slot& slot = m_slots[index];
        SAIGE_RHI_ASSERT(!slot.value.has_value(), "Free-list slot unexpectedly occupied");
        slot.value.emplace(std::forward<Args>(args)...);
        ++m_liveCount;
        return HandleType::fromParts(index, slot.generation);
    }

    /// Removes the object and returns it (for deferred destruction). Returns nullopt for stale handles.
    std::optional<T> release(HandleType handle)
    {
        Slot* slot = lookup(handle);
        if (slot == nullptr) {
            return std::nullopt;
        }
        std::optional<T> out(std::move(slot->value));
        retireSlot(*slot, handle.index());
        return out;
    }

    [[nodiscard]] T* get(HandleType handle) noexcept
    {
        Slot* slot = lookup(handle);
        return slot != nullptr ? &*slot->value : nullptr;
    }

    [[nodiscard]] const T* get(HandleType handle) const noexcept
    {
        const Slot* slot = lookup(handle);
        return slot != nullptr ? &*slot->value : nullptr;
    }

    [[nodiscard]] bool contains(HandleType handle) const noexcept { return lookup(handle) != nullptr; }

    /// Number of live objects.
    [[nodiscard]] uint32_t size() const noexcept { return m_liveCount; }
    [[nodiscard]] bool empty() const noexcept { return m_liveCount == 0; }
    /// Number of slots ever created (high-water mark).
    [[nodiscard]] uint32_t slotCount() const noexcept { return static_cast<uint32_t>(m_slots.size()); }
    [[nodiscard]] uint32_t maxSlots() const noexcept { return m_maxSlots; }

    /// Calls `fn(HandleType, T&)` for every live object, in slot order.
    template <typename Fn>
    void forEach(Fn&& fn)
    {
        for (uint32_t i = 0; i < m_slots.size(); ++i) {
            Slot& slot = m_slots[i];
            if (slot.value.has_value()) {
                fn(HandleType::fromParts(i, slot.generation), *slot.value);
            }
        }
    }

    /// Destroys all objects. Every outstanding handle becomes stale.
    void clear()
    {
        for (uint32_t i = 0; i < m_slots.size(); ++i) {
            Slot& slot = m_slots[i];
            if (slot.value.has_value()) {
                slot.value.reset();
                retireSlot(slot, i);
            }
        }
    }

private:
    struct Slot {
        std::optional<T> value;
        uint32_t generation = 1;
    };

    [[nodiscard]] Slot* lookup(HandleType handle) noexcept
    {
        return const_cast<Slot*>(std::as_const(*this).lookup(handle));
    }

    [[nodiscard]] const Slot* lookup(HandleType handle) const noexcept
    {
        if (!handle.isValid() || handle.index() >= m_slots.size()) {
            return nullptr;
        }
        const Slot& slot = m_slots[handle.index()];
        if (!slot.value.has_value() || slot.generation != handle.generation()) {
            return nullptr;
        }
        return &slot;
    }

    void retireSlot(Slot& slot, uint32_t index)
    {
        slot.value.reset();
        slot.generation = HandleType::nextGeneration(slot.generation);
        m_freeList.push_back(index);
        --m_liveCount;
    }

    std::vector<Slot> m_slots;
    std::deque<uint32_t> m_freeList;
    uint32_t m_liveCount = 0;
    uint32_t m_maxSlots;
};

} // namespace saige::rhi
