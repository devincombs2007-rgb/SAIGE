// A stateless, platform-independent hash: the same inputs give the same numbers everywhere, so every environment model is deterministic and replayable from a seed.
#pragma once

#include <cstdint>

namespace saige::env {

[[nodiscard]] constexpr std::uint64_t mix64(std::uint64_t z) noexcept
{
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

[[nodiscard]] constexpr std::uint64_t hash_combine(std::uint64_t a, std::uint64_t b) noexcept { return mix64(a ^ (mix64(b) + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2))); }

/// A uniform number in [0, 1) from a 64-bit hash (53 random bits).
[[nodiscard]] constexpr double unit(std::uint64_t h) noexcept { return static_cast<double>(h >> 11) * (1.0 / 9007199254740992.0); }

} // namespace saige::env
