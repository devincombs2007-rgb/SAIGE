#pragma once

#include "saige/chunk_stream.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace saige {

struct ChunkManifest {
    std::vector<ChunkSource> sources;
    std::optional<ChunkCoord> origin;
    Vec3 checkpoint{4, 4, 0};
};

struct CourseVehicleCheckpoint {
    ChunkCoord cell;
    Vec3 position;
    float heading{};
};

struct CourseCheckpoint {
    ChunkCoord cell;
    Vec3 feet;
    std::optional<CourseVehicleCheckpoint> vehicle;
};

/// A separate versioned gameplay save. Feet are relative to the logical cell.
[[nodiscard]] CourseCheckpoint load_course_checkpoint(const std::filesystem::path& path);
void save_course_checkpoint(const CourseCheckpoint& checkpoint, const std::filesystem::path& path);

/// Version 2 adds a grid origin and checkpoint in that cell's local coordinates.
[[nodiscard]] ChunkManifest read_chunk_manifest(const std::filesystem::path& path);
[[nodiscard]] ChunkCollision load_chunk_geometry(const std::filesystem::path& path);

/// Read a chunk manifest and return its sources in coordinate order.
[[nodiscard]] std::vector<ChunkSource> load_chunk_manifest(const std::filesystem::path& path);

/// Read ordered axis-aligned boxes from a version 1 collision file in simulation coordinates.
[[nodiscard]] std::vector<Aabb> load_chunk_collision(const std::filesystem::path& path);

/// Map a finite world position to its Z-up grid cell using a positive cell size.
[[nodiscard]] ChunkCoord chunk_containing(Vec3 position, double cell_size);

/// Return the chunks within a square around the center. Results are sorted and can be passed to ChunkStream::request.
[[nodiscard]] std::vector<ChunkSource> select_chunks(std::span<const ChunkSource> sources, ChunkCoord center,
                                                    std::uint32_t radius);

} // namespace saige
