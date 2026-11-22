#pragma once

#include "saige/chunk_stream.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace saige {

/// Read a version 1 chunk manifest and return its sources in coordinate order.
[[nodiscard]] std::vector<ChunkSource> load_chunk_manifest(const std::filesystem::path& path);

/// Return the chunks within a square around the center. Results are sorted and can be passed to ChunkStream::request.
[[nodiscard]] std::vector<ChunkSource> select_chunks(std::span<const ChunkSource> sources, ChunkCoord center,
                                                    std::uint32_t radius);

} // namespace saige
