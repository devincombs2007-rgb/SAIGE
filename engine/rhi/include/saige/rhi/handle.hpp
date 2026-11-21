// SAIGE RHI — 32-bit generational handles.
#pragma once

#include "saige/rhi/config.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace saige::rhi {

/// Strongly typed 32-bit generational handle.
///
/// Layout: `[generation : 12 bits | index : 20 bits]`.
/// - `index` addresses a slot in a backend-owned pool (up to 1,048,576 live objects per pool).
/// - `generation` increments every time a slot is reused, so stale handles are detected. Generation 0
///   is reserved, which makes the default-constructed handle (raw value 0) always invalid. Generations
///   cycle through 1..4095.
///
/// Handles are trivially copyable, comparable and hashable. They carry no ownership.
template <typename Tag>
class Handle {
public:
    static constexpr uint32_t index_bits = 20;
    static constexpr uint32_t generation_bits = 12;
    static constexpr uint32_t max_index = (1u << index_bits) - 1u;
    static constexpr uint32_t max_generation = (1u << generation_bits) - 1u;
    static constexpr uint32_t max_slots = max_index + 1u;

    constexpr Handle() noexcept = default;

    [[nodiscard]] static constexpr Handle from_parts(uint32_t index, uint32_t generation) noexcept
    {
        SAIGE_RHI_ASSERT(index <= max_index, "Handle index out of range");
        SAIGE_RHI_ASSERT(generation <= max_generation, "Handle generation out of range");
        Handle h;
        h.value_ = (generation << index_bits) | (index & max_index);
        return h;
    }

    [[nodiscard]] static constexpr Handle from_raw(uint32_t raw) noexcept
    {
        Handle h;
        h.value_ = raw;
        return h;
    }

    [[nodiscard]] constexpr uint32_t index() const noexcept { return value_ & max_index; }
    [[nodiscard]] constexpr uint32_t generation() const noexcept { return value_ >> index_bits; }
    [[nodiscard]] constexpr uint32_t raw() const noexcept { return value_; }

    /// True if the handle was produced by a pool. It may still be stale; ask the pool to be sure.
    [[nodiscard]] constexpr bool is_valid() const noexcept { return generation() != 0; }
    constexpr explicit operator bool() const noexcept { return is_valid(); }

    friend constexpr bool operator==(Handle, Handle) noexcept = default;

    /// Next generation after `g`, skipping the reserved value 0.
    [[nodiscard]] static constexpr uint32_t next_generation(uint32_t g) noexcept
    {
        return g >= max_generation ? 1u : g + 1u;
    }

private:
    uint32_t value_ = 0;
};

// Canonical handle types. The tags are incomplete types used only for type distinction.
using BufferHandle = Handle<struct BufferTag>;
using TextureHandle = Handle<struct TextureTag>;
using TextureViewHandle = Handle<struct TextureViewTag>;
using SamplerHandle = Handle<struct SamplerTag>;
using ShaderHandle = Handle<struct ShaderTag>;
using PipelineHandle = Handle<struct PipelineTag>;
using SwapchainHandle = Handle<struct SwapchainTag>;
using AccelerationStructureHandle = Handle<struct AccelerationStructureTag>;

static_assert(sizeof(BufferHandle) == 4, "Handles must stay 32-bit");

} // namespace saige::rhi

template <typename Tag>
struct std::hash<saige::rhi::Handle<Tag>> {
    [[nodiscard]] std::size_t operator()(saige::rhi::Handle<Tag> h) const noexcept
    {
        return std::hash<uint32_t>{}(h.raw());
    }
};
