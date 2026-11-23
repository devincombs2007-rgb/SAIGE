#pragma once

#include "desktop_game.hpp"
#include "saige/chunk_manifest.hpp"

#include <algorithm>
#include <array>
#include <iostream>

namespace saige::sample {

class StreamedCourse final : public IGame {
public:
    StreamedCourse(World& world, IDesktopPlatform& platform, const std::filesystem::path& manifest)
        : platform_(platform), sources_(load_chunk_manifest(manifest)) {
        const auto spawn_chunk = std::ranges::find(sources_, reset_cell, &ChunkSource::coord);
        if (spawn_chunk == sources_.end()) throw std::invalid_argument("Course manifest needs a reset chunk at [0, 0]");
        for (const auto& source : sources_) {
            // Leave room for the capsule, jump and boundary guards inside the local physics envelope.
            if (source.collision.empty() || source.coord.x < -31 || source.coord.x > 30 ||
                source.coord.y < -31 || source.coord.y > 30) {
                throw std::invalid_argument("Course chunks need collision files and coordinates in [-31, 30]");
            }
        }
        const auto initial = std::span(&*spawn_chunk, 1);
        stream_.request(initial);
        stream_.stage_next();
        CapsuleCharacter controller({4, 4, 0}, candidate_geometry(initial));
        if (!controller.state().grounded) throw std::invalid_argument("Course reset point must have walkable ground");
        game_.emplace(world, platform_, std::move(controller));
        stream_.commit_frame();
    }

    void fixed_update(World& world, std::chrono::nanoseconds step) override {
        const bool reset = platform_.key_down(Key::r);
        const ChunkCoord center = reset ? reset_cell : chunk_containing(game_->course_position(), cell_size);
        try {
            if (!requested_center_ || center != *requested_center_ || (reset && !reset_held_)) {
                requested_center_ = center;
                requested_ = select_chunks(sources_, center, 1);
                const auto origin = std::ranges::lower_bound(requested_, reset_cell, {}, &ChunkSource::coord);
                if (origin == requested_.end() || origin->coord != reset_cell) {
                    const auto spawn = std::ranges::find(sources_, reset_cell, &ChunkSource::coord);
                    requested_.insert(origin, *spawn);
                }
                stream_.request(requested_);
            }

            stream_.stage_next(); // At most one new disk payload per fixed update.
            if (stream_.ready_to_commit()) {
                if (!game_->replace_course_collision(candidate_geometry(requested_))) {
                    throw std::runtime_error("Staged course collision overlaps the player or reset point");
                }
                // No intervening operation can invalidate readiness; commit is noexcept.
                stream_.commit_frame();
            }
        } catch (const std::exception& error) {
            stream_.cancel();
            std::clog << "SAIGE course streaming kept the active chunks: " << error.what() << '\n';
        }
        reset_held_ = reset;
        game_->fixed_update(world, step);
    }

    [[nodiscard]] std::optional<CameraView> camera_view() const override { return game_->camera_view(); }
    [[nodiscard]] std::size_t resident_count() const noexcept { return stream_.active_count(); }

private:
    [[nodiscard]] std::vector<Aabb> candidate_geometry(std::span<const ChunkSource> requested) const {
        std::vector<Aabb> geometry;
        for (const auto& chunk : requested) {
            const auto boxes = stream_.staged_collision(chunk.coord).value();
            geometry.insert(geometry.end(), boxes.begin(), boxes.end());

            // Guard the resident perimeter. A delayed or failed load cannot strand the player in a missing chunk.
            const float x = static_cast<float>(chunk.coord.x) * cell_size;
            const float y = static_cast<float>(chunk.coord.y) * cell_size;
            constexpr float edge = 0.125f;
            const std::array neighbors{
                ChunkCoord{chunk.coord.x - 1, chunk.coord.y}, ChunkCoord{chunk.coord.x + 1, chunk.coord.y},
                ChunkCoord{chunk.coord.x, chunk.coord.y - 1}, ChunkCoord{chunk.coord.x, chunk.coord.y + 1},
            };
            const std::array guards{
                Aabb{{x - edge, y, -8}, {x, y + cell_size, 8}},
                Aabb{{x + cell_size, y, -8}, {x + cell_size + edge, y + cell_size, 8}},
                Aabb{{x, y - edge, -8}, {x + cell_size, y, 8}},
                Aabb{{x, y + cell_size, -8}, {x + cell_size, y + cell_size + edge, 8}},
            };
            for (std::size_t side = 0; side < neighbors.size(); ++side) {
                if (std::ranges::find(requested, neighbors[side], &ChunkSource::coord) == requested.end()) {
                    geometry.push_back(guards[side]);
                }
            }
        }
        return geometry;
    }

    static constexpr float cell_size = 8;
    static constexpr ChunkCoord reset_cell{0, 0};
    IDesktopPlatform& platform_;
    std::vector<ChunkSource> sources_;
    ChunkStream stream_{{10}};
    std::optional<DesktopGame> game_;
    std::optional<ChunkCoord> requested_center_;
    std::vector<ChunkSource> requested_;
    bool reset_held_{};
};

} // namespace saige::sample
