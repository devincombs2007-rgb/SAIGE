#pragma once

#include <compare>
#include <cstdint>

namespace saige {

// A cell of the Z-up x/y chunk grid. (Moved here from chunk_stream.hpp, which still includes it: source compatible.)
struct ChunkCoord {
    std::int32_t x;
    std::int32_t y;
    constexpr auto operator<=>(const ChunkCoord&) const = default;
};

} // namespace saige
