#include "saige/rhi/bindless_allocator.h"

#include "saige/rhi/config.h"

namespace saige::rhi {

BindlessIndexAllocator::BindlessIndexAllocator(uint32_t capacity, uint32_t reservedCount)
    : m_capacity(capacity)
    , m_reservedCount(reservedCount < capacity ? reservedCount : capacity)
    , m_nextUnused(m_reservedCount)
    , m_states(capacity, State::Free)
{
    SAIGE_RHI_ASSERT(capacity < kInvalidIndex, "Bindless capacity must be below kInvalidIndex");
    for (uint32_t i = 0; i < m_reservedCount; ++i) {
        m_states[i] = State::Reserved;
    }
}

uint32_t BindlessIndexAllocator::allocate()
{
    uint32_t index = kInvalidIndex;
    if (!m_freeList.empty()) {
        index = m_freeList.front();
        m_freeList.pop_front();
    } else if (m_nextUnused < m_capacity) {
        index = m_nextUnused++;
    } else {
        return kInvalidIndex;
    }
    SAIGE_RHI_ASSERT(m_states[index] == State::Free, "Bindless free list corrupted");
    m_states[index] = State::Allocated;
    ++m_allocatedCount;
    return index;
}

bool BindlessIndexAllocator::release(uint32_t index, uint64_t retireAfter)
{
    if (index >= m_capacity || m_states[index] != State::Allocated) {
        return false;
    }
    m_states[index] = State::PendingRelease;
    --m_allocatedCount;
    m_pending.enqueue(index, retireAfter);
    return true;
}

uint32_t BindlessIndexAllocator::reclaim(uint64_t completedValue)
{
    const std::size_t n = m_pending.collect(completedValue, [this](uint32_t index) {
        SAIGE_RHI_ASSERT(m_states[index] == State::PendingRelease, "Bindless pending state corrupted");
        m_states[index] = State::Free;
        m_freeList.push_back(index);
    });
    return static_cast<uint32_t>(n);
}

bool BindlessIndexAllocator::isAllocated(uint32_t index) const noexcept
{
    return index < m_capacity && m_states[index] == State::Allocated;
}

} // namespace saige::rhi
