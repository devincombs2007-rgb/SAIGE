#pragma once

#include "desktop_game.hpp"
#include "saige/chunk_manifest.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <map>
#include <type_traits>

namespace saige::sample {

struct CourseAsset {
    AssetId id;
    const gfx::GltfAsset* model;
    std::filesystem::path source;
};

// Native host adapter. Staging may throw. Publication only adopts resources staged by this adapter.
class CourseSceneOutput {
public:
    virtual ~CourseSceneOutput() = default;
    // Prepare at most one new GPU asset. False keeps the active course in force and resumes next tick.
    virtual bool stage(std::span<const CourseAsset> assets) = 0;
    virtual void publish() noexcept = 0;
    virtual void discard() noexcept = 0;
};

class StreamedCourse final : public IGame {
public:
    StreamedCourse(World& world, IDesktopPlatform& platform, const std::filesystem::path& manifest,
                   CourseSceneOutput* output = nullptr)
        : platform_(platform), sources_(load_chunk_manifest(manifest)), output_(output) {
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
        if (output_) {
            World candidate = world;
            const auto assets = candidate_assets(initial);
            while (!output_->stage(assets)) {} // Finish the initial scene before entering the simulation loop.
            auto entities = compose_world(candidate, initial);
            publish_visuals(world, std::move(candidate), std::move(entities));
        }
        stream_.commit_frame();
    }

    void fixed_update(World& world, std::chrono::nanoseconds step) override {
        const bool reset = platform_.key_down(Key::r);
        const ChunkCoord center = reset ? reset_cell : chunk_containing(game_->course_position(), cell_size);
        try {
            update_candidate(world, center, reset);
        } catch (const std::exception& error) {
            if (output_) output_->discard();
            stream_.cancel();
            std::clog << "SAIGE course streaming kept the active chunks: " << error.what() << '\n';
        }
        reset_held_ = reset;
        game_->fixed_update(world, step);
    }

    [[nodiscard]] std::optional<CameraView> camera_view() const override { return game_->camera_view(); }
    [[nodiscard]] std::size_t resident_count() const noexcept { return stream_.active_count(); }

private:
    using ChunkEntities = std::map<ChunkCoord, std::vector<Entity>>;

    void update_candidate(World& world, ChunkCoord center, bool reset) {
        if (!requested_center_ || center != *requested_center_ || (reset && !reset_held_)) {
            if (output_) output_->discard();
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
        if (!stream_.ready_to_commit()) return;
        std::optional<World> candidate_world;
        ChunkEntities entities;
        if (output_) {
            const auto assets = candidate_assets(requested_);
            if (!output_->stage(assets)) return;
            candidate_world = world;
            entities = compose_world(*candidate_world, requested_);
        }
        if (!game_->replace_course_collision(candidate_geometry(requested_))) {
            throw std::runtime_error("Staged course collision overlaps the player or reset point");
        }
        if (candidate_world) publish_visuals(world, std::move(*candidate_world), std::move(entities));
        stream_.commit_frame();
    }

    [[nodiscard]] ChunkEntities compose_world(World& candidate, std::span<const ChunkSource> requested) const {
        ChunkEntities entities = chunk_entities_;
        for (auto it = entities.begin(); it != entities.end();) {
            if (std::ranges::find(requested, it->first, &ChunkSource::coord) == requested.end()) {
                for (const Entity entity : it->second) (void)candidate.destroy(entity);
                it = entities.erase(it);
            } else {
                ++it;
            }
        }

        for (const ChunkSource& chunk : requested) {
            const auto& project = stream_.staged_chunk(chunk.coord)->project();
            const auto [it, inserted] = entities.try_emplace(chunk.coord);
            if (!inserted) continue; // Keep the render identity of resident nodes.
            const auto matrices = resolve_project_matrices(project);
            for (std::size_t i = 0; i < project.nodes.size(); ++i) {
                auto matrix = matrices[i];
                matrix[12] += static_cast<float>(chunk.coord.x) * cell_size;
                matrix[13] += static_cast<float>(chunk.coord.y) * cell_size;
                const auto entity = candidate.create();
                if (!candidate.set_render_pose(entity, matrix, project.nodes[i].mesh)) {
                    throw std::invalid_argument("Invalid streamed chunk render transform");
                }
                it->second.push_back(entity);
            }
        }

        return entities;
    }

    [[nodiscard]] std::vector<CourseAsset> candidate_assets(std::span<const ChunkSource> requested) const {
        std::map<std::pair<std::uint64_t, std::uint64_t>, CourseAsset> unique_assets;
        for (const ChunkSource& chunk : requested) {
            const PreparedScene& prepared = *stream_.staged_chunk(chunk.coord);
            const auto& project = prepared.project();
            for (const auto& asset : project.assets) {
                const auto relative = std::filesystem::path(
                    std::u8string(asset.relative_path.begin(), asset.relative_path.end()));
                const auto source = (chunk.project.parent_path() / relative).lexically_normal();
                const auto [it, inserted] = unique_assets.try_emplace(
                    std::pair{asset.id.high, asset.id.low}, CourseAsset{asset.id, prepared.find_asset(asset.id), source});
                if (!inserted && it->second.source != source) {
                    throw std::invalid_argument("Streamed chunks bind the same asset ID to different files");
                }
            }

        }
        std::vector<CourseAsset> assets;
        assets.reserve(unique_assets.size());
        for (auto& entry : unique_assets) assets.push_back(std::move(entry.second));
        return assets;
    }

    void publish_visuals(World& world, World&& candidate, ChunkEntities&& entities) noexcept {
        static_assert(std::is_nothrow_move_assignable_v<World>);
        static_assert(std::is_nothrow_move_assignable_v<ChunkEntities>);
        output_->publish();
        world = std::move(candidate);
        chunk_entities_ = std::move(entities);
    }

    [[nodiscard]] std::vector<Aabb> candidate_geometry(std::span<const ChunkSource> requested) const {
        std::vector<Aabb> geometry;
        for (const auto& chunk : requested) {
            const auto boxes = stream_.staged_collision(chunk.coord).value();
            if (std::ranges::any_of(boxes, [](const Aabb& box) { return box.max.z > max_ground_height; })) {
                throw std::invalid_argument("Course collision exceeds the supported gameplay height");
            }
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
                Aabb{{x - edge, y, -local_extent}, {x, y + cell_size, local_extent}},
                Aabb{{x + cell_size, y, -local_extent}, {x + cell_size + edge, y + cell_size, local_extent}},
                Aabb{{x, y - edge, -local_extent}, {x + cell_size, y, local_extent}},
                Aabb{{x, y + cell_size, -local_extent}, {x + cell_size, y + cell_size + edge, local_extent}},
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
    static constexpr float local_extent = 256;
    static constexpr float max_ground_height = 240; // Leave room for capsule height and jumping.
    static constexpr ChunkCoord reset_cell{0, 0};
    IDesktopPlatform& platform_;
    std::vector<ChunkSource> sources_;
    CourseSceneOutput* output_;
    ChunkEntities chunk_entities_;
    ChunkStream stream_{{10}};
    std::optional<DesktopGame> game_;
    std::optional<ChunkCoord> requested_center_;
    std::vector<ChunkSource> requested_;
    bool reset_held_{};
};

} // namespace saige::sample
