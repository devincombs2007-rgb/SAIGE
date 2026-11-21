// SAIGE RHI — generational object pool addressed by Handle<Tag>.
#pragma once

#include "saige/rhi/config.hpp"
#include "saige/rhi/handle.hpp"

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

    explicit HandlePool(uint32_t max_slots = HandleType::max_slots) noexcept
        : max_slots_(max_slots < HandleType::max_slots ? max_slots : HandleType::max_slots)
    {
    }

    /// Constructs a `T` in a free slot. Returns an invalid handle if the pool is full.
    template <typename... Args>
    [[nodiscard]] HandleType emplace(Args&&... args)
    {
        uint32_t index = 0;
        if (!free_list_.empty()) {
            index = free_list_.front();
            free_list_.pop_front();
        } else {
            if (slots_.size() >= max_slots_) {
                return HandleType{};
            }
            index = static_cast<uint32_t>(slots_.size());
            slots_.emplace_back();
        }

        Slot& slot = slots_[index];
        SAIGE_RHI_ASSERT(!slot.value.has_value(), "Free-list slot unexpectedly occupied");
        slot.value.emplace(std::forward<Args>(args)...);
        ++live_count_;
        return HandleType::from_parts(index, slot.generation);
    }

    /// Removes the object and returns it (for deferred destruction). Returns nullopt for stale handles.
    std::optional<T> release(HandleType handle)
    {
        Slot* slot = lookup(handle);
        if (slot == nullptr) {
            return std::nullopt;
        }
        std::optional<T> out(std::move(slot->value));
        retire_slot(*slot, handle.index());
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
    [[nodiscard]] uint32_t size() const noexcept { return live_count_; }
    [[nodiscard]] bool empty() const noexcept { return live_count_ == 0; }
    /// Number of slots ever created (high-water mark).
    [[nodiscard]] uint32_t slot_count() const noexcept { return static_cast<uint32_t>(slots_.size()); }
    [[nodiscard]] uint32_t max_slots() const noexcept { return max_slots_; }

    /// Calls `fn(HandleType, T&)` for every live object, in slot order.
    template <typename Fn>
    void for_each(Fn&& fn)
    {
        for (uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            if (slot.value.has_value()) {
                fn(HandleType::from_parts(i, slot.generation), *slot.value);
            }
        }
    }

    /// Destroys all objects. Every outstanding handle becomes stale.
    void clear()
    {
        for (uint32_t i = 0; i < slots_.size(); ++i) {
            Slot& slot = slots_[i];
            if (slot.value.has_value()) {
                slot.value.reset();
                retire_slot(slot, i);
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
        if (!handle.is_valid() || handle.index() >= slots_.size()) {
            return nullptr;
        }
        const Slot& slot = slots_[handle.index()];
        if (!slot.value.has_value() || slot.generation != handle.generation()) {
            return nullptr;
        }
        return &slot;
    }

    void retire_slot(Slot& slot, uint32_t index)
    {
        slot.value.reset();
        slot.generation = HandleType::next_generation(slot.generation);
        free_list_.push_back(index);
        --live_count_;
    }

    std::vector<Slot> slots_;
    std::deque<uint32_t> free_list_;
    uint32_t live_count_ = 0;
    uint32_t max_slots_;
};

} // namespace saige::rhi
