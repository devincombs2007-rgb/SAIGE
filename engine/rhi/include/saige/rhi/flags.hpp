// SAIGE RHI — type-safe bit flag operators for scoped enums.
#pragma once

#include <type_traits>

/// Enables bitwise operators for a scoped enum used as a flag set.
/// Usage (at namespace scope, same namespace as the enum): SAIGE_RHI_ENUM_FLAGS(BufferUsage)
#define SAIGE_RHI_ENUM_FLAGS(E)                                                                    \
    [[nodiscard]] constexpr E operator|(E a, E b) noexcept                                         \
    {                                                                                              \
        using U = std::underlying_type_t<E>;                                                       \
        return static_cast<E>(static_cast<U>(a) | static_cast<U>(b));                              \
    }                                                                                              \
    [[nodiscard]] constexpr E operator&(E a, E b) noexcept                                         \
    {                                                                                              \
        using U = std::underlying_type_t<E>;                                                       \
        return static_cast<E>(static_cast<U>(a) & static_cast<U>(b));                              \
    }                                                                                              \
    [[nodiscard]] constexpr E operator^(E a, E b) noexcept                                         \
    {                                                                                              \
        using U = std::underlying_type_t<E>;                                                       \
        return static_cast<E>(static_cast<U>(a) ^ static_cast<U>(b));                              \
    }                                                                                              \
    [[nodiscard]] constexpr E operator~(E a) noexcept                                              \
    {                                                                                              \
        using U = std::underlying_type_t<E>;                                                       \
        return static_cast<E>(~static_cast<U>(a));                                                 \
    }                                                                                              \
    constexpr E& operator|=(E& a, E b) noexcept { return a = a | b; }                              \
    constexpr E& operator&=(E& a, E b) noexcept { return a = a & b; }                              \
    constexpr E& operator^=(E& a, E b) noexcept { return a = a ^ b; }

namespace saige::rhi {

/// True if any bit of `bits` is set in `value`.
template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr bool has_any(E value, E bits) noexcept
{
    using U = std::underlying_type_t<E>;
    return (static_cast<U>(value) & static_cast<U>(bits)) != 0;
}

/// True if every bit of `bits` is set in `value`.
template <typename E>
    requires std::is_enum_v<E>
[[nodiscard]] constexpr bool has_all(E value, E bits) noexcept
{
    using U = std::underlying_type_t<E>;
    return (static_cast<U>(value) & static_cast<U>(bits)) == static_cast<U>(bits);
}

} // namespace saige::rhi
