#pragma once

#include "saige/prepared_scene.hpp"

#include <compare>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>

namespace saige {

struct ChunkCoord {
    std::int32_t x;
    std::int32_t y;
    constexpr auto operator<=>(const ChunkCoord&) const = default;
};

struct ChunkSource {
    ChunkCoord coord;
    std::filesystem::path project;
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

    // Replace the pending set. Reuse active chunks with the same coordinate and path.
    void request(std::span<const ChunkSource> chunks);
    // Stage at most one chunk, in coordinate order. An import failure abandons the pending set and throws.
    bool stage_next();
    void cancel() noexcept;
    // Publish only a complete set. Borrowed active scene pointers expire at the next successful commit.
    bool commit_frame() noexcept;

    [[nodiscard]] const PreparedScene* active_chunk(ChunkCoord coord) const;
    [[nodiscard]] std::size_t active_count() const noexcept { return active_.size(); }
    [[nodiscard]] std::size_t retained_asset_bytes() const noexcept;

private:
    struct Chunk {
        std::filesystem::path project;
        std::shared_ptr<const PreparedScene> scene;
    };
    using Chunks = std::map<ChunkCoord, Chunk>;

    ChunkStreamLimits limits_;
    Chunks active_;
    std::optional<Chunks> pending_;
};

} // namespace saige
