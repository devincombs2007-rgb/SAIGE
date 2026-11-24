#include "saige/chunk_manifest.hpp"

#include <algorithm>
#include <chrono>
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
        Chunk chunk{source.project, source.collision};
        const auto matches = [&source](const Chunk& item) {
            return item.project == source.project && item.collision == source.collision && item.scene;
        };
        const auto active = active_.find(source.coord);
        if (active != active_.end() && matches(active->second)) {
            chunk = active->second;
        } else if (pending_) {
            const auto staged = pending_->find(source.coord);
            if (staged != pending_->end() && matches(staged->second)) chunk = staged->second;
        }
        if (!candidate.emplace(source.coord, std::move(chunk)).second) {
            throw std::invalid_argument("Duplicate chunk coordinates");
        }
    }
    if (loading_) {
        const auto still_requested = candidate.find(loading_->coord);
        if (still_requested == candidate.end() || still_requested->second.project != loading_->project ||
            still_requested->second.collision != loading_->collision) loading_->abandoned = true;
    }
    pending_ = std::move(candidate);
}

ChunkStream::Chunk ChunkStream::prepare(Chunk chunk, std::size_t remaining) {
    if (!chunk.collision.empty()) {
        chunk.geometry = std::make_shared<const ChunkCollision>(load_chunk_geometry(chunk.collision));
    }
    chunk.scene = std::make_shared<const PreparedScene>(chunk.project, remaining);
    return chunk;
}

bool ChunkStream::stage_next() {
    if (loading_) throw std::logic_error("Cannot stage synchronously while a background chunk is in flight");
    if (!pending_) return false;
    const auto next = std::ranges::find_if(*pending_, [](const auto& item) { return !item.second.scene; });
    if (next == pending_->end()) return false;
    try {
        const auto remaining = limits_.max_retained_asset_bytes - retained_asset_bytes();
        next->second = prepare(next->second, remaining);
    } catch (...) {
        pending_.reset();
        throw;
    }
    return true;
}

void ChunkStream::stage_background() {
    if (loading_) {
        if (loading_->future.wait_for(std::chrono::seconds::zero()) != std::future_status::ready) return;
        auto finished = std::move(*loading_);
        loading_.reset();
        if (finished.abandoned || !pending_) return;
        const auto target = pending_->find(finished.coord);
        if (target == pending_->end() || target->second.scene ||
            target->second.project != finished.project || target->second.collision != finished.collision) return;
        try {
            target->second = finished.future.get();
        } catch (...) {
            pending_.reset();
            throw;
        }
        return;
    }
    if (!pending_) return;
    const auto next = std::ranges::find_if(*pending_, [](const auto& item) { return !item.second.scene; });
    if (next == pending_->end()) return;
    try {
        const auto remaining = limits_.max_retained_asset_bytes - retained_asset_bytes();
        Loading task{next->first, next->second.project, next->second.collision, {}};
        task.future = std::async(std::launch::async, [chunk = next->second, remaining]() mutable {
            return prepare(std::move(chunk), remaining);
        });
        loading_ = std::move(task);
    } catch (...) {
        pending_.reset();
        throw;
    }
}

void ChunkStream::cancel() noexcept {
    if (loading_) loading_->abandoned = true;
    pending_.reset();
}

bool ChunkStream::ready_to_commit() const noexcept {
    return pending_ && std::ranges::all_of(*pending_, [](const auto& item) { return bool(item.second.scene); });
}

const PreparedScene* ChunkStream::staged_chunk(ChunkCoord coord) const noexcept {
    if (!pending_) return nullptr;
    const auto it = pending_->find(coord);
    return it == pending_->end() ? nullptr : it->second.scene.get();
}

std::optional<std::span<const Aabb>> ChunkStream::staged_collision(ChunkCoord coord) const noexcept {
    if (!pending_) return {};
    const auto it = pending_->find(coord);
    if (it == pending_->end() || !it->second.geometry) return {};
    return std::span<const Aabb>(it->second.geometry->boxes);
}

std::optional<std::span<const Triangle>> ChunkStream::staged_triangles(ChunkCoord coord) const noexcept {
    if (!pending_) return {};
    const auto it = pending_->find(coord);
    if (it == pending_->end() || !it->second.geometry) return {};
    return std::span<const Triangle>(it->second.geometry->triangles);
}

bool ChunkStream::commit_frame() noexcept {
    if (!ready_to_commit()) return false;
    active_.swap(*pending_);
    pending_.reset();
    return true;
}

const PreparedScene* ChunkStream::active_chunk(ChunkCoord coord) const {
    const auto it = active_.find(coord);
    return it == active_.end() ? nullptr : it->second.scene.get();
}

const ChunkCollision* ChunkStream::active_geometry(ChunkCoord coord) const noexcept {
    const auto it = active_.find(coord);
    return it == active_.end() ? nullptr : it->second.geometry.get();
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
