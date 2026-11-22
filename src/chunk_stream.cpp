#include "saige/chunk_stream.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace saige {

ChunkStream::ChunkStream(ChunkStreamLimits limits) : limits_(limits) {}

void ChunkStream::request(std::span<const ChunkSource> chunks) {
    if (chunks.size() > limits_.max_chunks) {
        throw std::invalid_argument("Chunk request exceeds the chunk count limit");
    }
    Chunks candidate;
    for (const auto& source : chunks) {
        if (source.project.empty()) {
            throw std::invalid_argument("A chunk project path cannot be empty");
        }
        const auto active = active_.find(source.coord);
        std::shared_ptr<const PreparedScene> scene;
        if (active != active_.end() && active->second.project == source.project) {
            scene = active->second.scene;
        }
        if (!candidate.emplace(source.coord, Chunk{source.project, std::move(scene)}).second) {
            throw std::invalid_argument("Duplicate chunk coordinates");
        }
    }
    pending_ = std::move(candidate);
}

bool ChunkStream::stage_next() {
    if (!pending_) return false;
    const auto next = std::ranges::find_if(*pending_, [](const auto& item) { return !item.second.scene; });
    if (next == pending_->end()) return false;
    try {
        const auto remaining = limits_.max_retained_asset_bytes - retained_asset_bytes();
        next->second.scene = std::make_shared<const PreparedScene>(next->second.project, remaining);
    } catch (...) {
        pending_.reset();
        throw;
    }
    return true;
}

void ChunkStream::cancel() noexcept {
    pending_.reset();
}

bool ChunkStream::commit_frame() noexcept {
    if (!pending_ || std::ranges::any_of(*pending_, [](const auto& item) { return !item.second.scene; })) {
        return false;
    }
    active_.swap(*pending_);
    pending_.reset();
    return true;
}

const PreparedScene* ChunkStream::active_chunk(ChunkCoord coord) const {
    const auto it = active_.find(coord);
    return it == active_.end() ? nullptr : it->second.scene.get();
}

std::size_t ChunkStream::retained_asset_bytes() const noexcept {
    std::size_t bytes = 0;
    for (const auto& [coord, chunk] : active_) {
        bytes += chunk.scene->asset_payload_bytes();
    }
    if (pending_) {
        for (const auto& [coord, chunk] : *pending_) {
            const auto active = active_.find(coord);
            if (chunk.scene && (active == active_.end() || active->second.scene != chunk.scene)) {
                bytes += chunk.scene->asset_payload_bytes();
            }
        }
    }
    return bytes;
}

} // namespace saige
