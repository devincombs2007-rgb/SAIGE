#pragma once

#include "saige/chunk_stream.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace saige {

/// Read a version 1 chunk manifest and return its sources in coordinate order.
[[nodiscard]] std::vector<ChunkSource> load_chunk_manifest(const std::filesystem::path& path);

/// Map a finite world position to its Z-up grid cell using a positive cell size.
[[nodiscard]] ChunkCoord chunk_containing(Vec3 position, double cell_size);

/// Return the chunks within a square around the center. Results are sorted and can be passed to ChunkStream::request.
[[nodiscard]] std::vector<ChunkSource> select_chunks(std::span<const ChunkSource> sources, ChunkCoord center,
                                                    std::uint32_t radius);

} // namespace saige
