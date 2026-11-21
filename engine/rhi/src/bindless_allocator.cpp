#include "saige/rhi/bindless_allocator.hpp"

#include "saige/rhi/config.hpp"

namespace saige::rhi {

BindlessIndexAllocator::BindlessIndexAllocator(uint32_t capacity, uint32_t reserved_count)
    : capacity_(capacity)
    , reserved_count_(reserved_count < capacity ? reserved_count : capacity)
    , next_unused_(reserved_count_)
    , states_(capacity, State::Free)
{
    SAIGE_RHI_ASSERT(capacity < invalid_index, "Bindless capacity must be below invalid_index");
    for (uint32_t i = 0; i < reserved_count_; ++i) {
        states_[i] = State::Reserved;
    }
}

uint32_t BindlessIndexAllocator::allocate()
{
    uint32_t index = invalid_index;
    if (!free_list_.empty()) {
        index = free_list_.front();
        free_list_.pop_front();
    } else if (next_unused_ < capacity_) {
        index = next_unused_++;
    } else {
        return invalid_index;
    }
    SAIGE_RHI_ASSERT(states_[index] == State::Free, "Bindless free list corrupted");
    states_[index] = State::Allocated;
    ++allocated_count_;
    return index;
}

bool BindlessIndexAllocator::release(uint32_t index, uint64_t retire_after)
{
    if (index >= capacity_ || states_[index] != State::Allocated) {
        return false;
    }
    states_[index] = State::PendingRelease;
    --allocated_count_;
    pending_.enqueue(index, retire_after);
    return true;
}

uint32_t BindlessIndexAllocator::reclaim(uint64_t completed_value)
{
    const std::size_t n = pending_.collect(completed_value, [this](uint32_t index) {
        SAIGE_RHI_ASSERT(states_[index] == State::PendingRelease, "Bindless pending state corrupted");
        states_[index] = State::Free;
        free_list_.push_back(index);
    });
    return static_cast<uint32_t>(n);
}

bool BindlessIndexAllocator::is_allocated(uint32_t index) const noexcept
{
    return index < capacity_ && states_[index] == State::Allocated;
}

} // namespace saige::rhi
