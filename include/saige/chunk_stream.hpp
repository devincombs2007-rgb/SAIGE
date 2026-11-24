#pragma once

#include "saige/capsule_triangle.hpp"
#include "saige/prepared_scene.hpp"

#include <compare>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace saige {

struct ChunkCoord {
    std::int32_t x;
    std::int32_t y;
    constexpr auto operator<=>(const ChunkCoord&) const = default;
};

struct ChunkSource {
    ChunkCoord coord;
    std::filesystem::path project;
    std::filesystem::path collision{}; // An empty path requests a scene with no collision file.
};

struct ChunkCollision {
    std::vector<Aabb> boxes;
    std::vector<Triangle> triangles;
};

struct ChunkStreamLimits {
    std::size_t max_chunks{16}; // Per active or requested set.
    std::size_t max_retained_asset_bytes{std::size_t{256} << 20}; // Active and newly staged payloads.
};

// Synchronous CPU preparation. Drive from the runtime thread between frames.
class ChunkStream {
public:
    explicit ChunkStream(ChunkStreamLimits limits = {});
    ChunkStream(const ChunkStream&) = delete;
    ChunkStream& operator=(const ChunkStream&) = delete;

    // Replace the pending set. Reuse active or already staged chunks with the same coordinate and file paths.
    void request(std::span<const ChunkSource> chunks);
    // Stage at most one chunk, in coordinate order. An import failure abandons the pending set and throws.
    bool stage_next();
    void cancel() noexcept;
    // Inspect the pending set before publishing coupled resources such as static collision.
    // Staged pointers are borrowed until the next request, cancellation, failed stage, or commit.
    [[nodiscard]] bool ready_to_commit() const noexcept;
    [[nodiscard]] const PreparedScene* staged_chunk(ChunkCoord coord) const noexcept;
    // A present empty span is an authored empty file. The same lifetime rules as staged_chunk apply.
    [[nodiscard]] std::optional<std::span<const Aabb>> staged_collision(ChunkCoord coord) const noexcept;
    [[nodiscard]] std::optional<std::span<const Triangle>> staged_triangles(ChunkCoord coord) const noexcept;
    // Publish only a complete set. Borrowed active scene pointers expire at the next successful commit.
    bool commit_frame() noexcept;

    [[nodiscard]] const PreparedScene* active_chunk(ChunkCoord coord) const;
    [[nodiscard]] std::size_t active_count() const noexcept { return active_.size(); }
    [[nodiscard]] std::size_t retained_asset_bytes() const noexcept;

private:
    struct Chunk {
        std::filesystem::path project;
        std::filesystem::path collision;
        std::shared_ptr<const PreparedScene> scene{};
        std::shared_ptr<const ChunkCollision> geometry{};
    };
    using Chunks = std::map<ChunkCoord, Chunk>;

    ChunkStreamLimits limits_;
    Chunks active_;
    std::optional<Chunks> pending_;
};

} // namespace saige
