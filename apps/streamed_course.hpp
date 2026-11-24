#pragma once

#include "desktop_game.hpp"
#include "saige/chunk_manifest.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
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
                   CourseSceneOutput* output = nullptr, CapsuleCharacterConfig config = {}, bool enable_vehicle = false, bool background = false)
        : platform_(platform), output_(output), background_(background) {
        auto world_manifest = read_chunk_manifest(manifest);
        origin_ = world_manifest.origin.value_or(ChunkCoord{0, 0});
        checkpoint_cell_ = pending_origin_ = origin_;
        const Vec3 checkpoint = world_manifest.checkpoint;
        if (checkpoint.x <= 0 || checkpoint.x >= cell_size || checkpoint.y <= 0 || checkpoint.y >= cell_size ||
            checkpoint.z <= -8 || checkpoint.z > max_ground_height) {
            throw std::invalid_argument("Course checkpoint is outside its reset cell");
        }
        chunk_local_collision_ = world_manifest.origin.has_value();
        sources_ = std::move(world_manifest.sources);
        for (const auto& source : sources_) {
            if (source.collision.empty() || (!chunk_local_collision_ && !fits_frame(source.coord, origin_))) {
                throw std::invalid_argument("Course chunks need collision files and a representable legacy cell");
            }
        }
        const auto spawn_chunk = std::ranges::find(sources_, checkpoint_cell_, &ChunkSource::coord);
        if (spawn_chunk == sources_.end()) throw std::invalid_argument("Course manifest needs a reset chunk at its origin");
        const auto initial = std::span(&*spawn_chunk, 1);
        resident_coords_ = {checkpoint_cell_};
        stream_.request(initial);
        stream_.stage_next();
        const auto geometry = candidate_geometry(initial);
        CapsuleCharacter controller(checkpoint, geometry.boxes, geometry.triangles, config);
        if (!controller.state().grounded) throw std::invalid_argument("Course reset point must have walkable ground");
        game_.emplace(world, platform_, std::move(controller), geometry.boxes, geometry.triangles, enable_vehicle);
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
        const ChunkCoord center = reset ? checkpoint_cell_ : cell_at(game_->course_position());
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

    [[nodiscard]] bool driving() const noexcept { return game_->driving(); }
    [[nodiscard]] std::pair<std::int64_t, std::int64_t> absolute_cell() const {
        const auto cell = cell_at(game_->course_position());
        return {cell.x, cell.y};
    }
    [[nodiscard]] std::uint64_t origin_epoch() const noexcept { return origin_epoch_; }

private:
    using ChunkEntities = std::map<ChunkCoord, std::vector<Entity>>;

    [[nodiscard]] ChunkCoord cell_at(Vec3 position) const {
        const auto local = chunk_containing(position, cell_size);
        constexpr auto low = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
        constexpr auto high = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
        return {static_cast<std::int32_t>(std::clamp(static_cast<std::int64_t>(origin_.x) + local.x, low, high)),
                static_cast<std::int32_t>(std::clamp(static_cast<std::int64_t>(origin_.y) + local.y, low, high))};
    }

    [[nodiscard]] static Vec3 offset(ChunkCoord cell, ChunkCoord origin) {
        return {static_cast<float>(static_cast<std::int64_t>(cell.x) - origin.x) * cell_size,
                static_cast<float>(static_cast<std::int64_t>(cell.y) - origin.y) * cell_size, 0};
    }

    [[nodiscard]] static bool fits_frame(ChunkCoord cell, ChunkCoord origin) {
        const auto x = static_cast<std::int64_t>(cell.x) - origin.x;
        const auto y = static_cast<std::int64_t>(cell.y) - origin.y;
        return x >= -31 && x <= 30 && y >= -31 && y <= 30;
    }

    void update_candidate(World& world, ChunkCoord center, bool reset) {
        const auto parked = game_->vehicle_position();
        const auto vehicle_cell = parked ? std::optional(cell_at(*parked)) : std::nullopt;
        if (!requested_center_ || center != *requested_center_ || vehicle_cell != requested_vehicle_cell_ || (reset && !reset_held_)) {
            if (output_) output_->discard();
            requested_center_ = center;
            requested_vehicle_cell_ = vehicle_cell;
            requested_ = select_chunks(sources_, center, 1);
            retain_source(checkpoint_cell_);
            if (vehicle_cell) retain_source(*vehicle_cell);
            pending_origin_ = origin_;
            const auto movement = offset(center, origin_);
            if (chunk_local_collision_ && (std::abs(movement.x) >= 32 || std::abs(movement.y) >= 32 || reset) &&
                fits_frame(cell_at(game_->course_position()), center) &&
                std::ranges::all_of(requested_, [&](const auto& source) { return fits_frame(source.coord, center); })) {
                pending_origin_ = center;
            }
            std::erase_if(requested_, [&](const auto& source) { return !fits_frame(source.coord, pending_origin_); });
            pending_coords_.clear();
            for (const auto& source : requested_) pending_coords_.push_back(source.coord);
            if (pending_coords_ == resident_coords_ && pending_origin_ == origin_) {
                stream_.cancel();
                return;
            }
            stream_.request(requested_);
        }
        if (background_) stream_.stage_background();
        else stream_.stage_next();
        if (!stream_.ready_to_commit()) return;
        std::optional<World> candidate_world;
        ChunkEntities entities;
        if (output_) {
            const auto assets = candidate_assets(requested_);
            if (!output_->stage(assets)) return;
            candidate_world = world;
            entities = compose_world(*candidate_world, requested_);
        }
        const auto geometry = candidate_geometry(requested_);
        if (!game_->replace_course_collision(geometry.boxes, geometry.triangles, offset(origin_, pending_origin_))) {
            throw std::runtime_error("Staged course collision overlaps the player or reset point");
        }
        if (candidate_world) publish_visuals(world, std::move(*candidate_world), std::move(entities));
        stream_.commit_frame();
        resident_coords_.swap(pending_coords_);
        if (origin_ != pending_origin_) ++origin_epoch_;
        origin_ = pending_origin_;
    }

    void retain_source(ChunkCoord coord) {
        const auto entry = std::ranges::lower_bound(requested_, coord, {}, &ChunkSource::coord);
        if (entry != requested_.end() && entry->coord == coord) return;
        const auto source = std::ranges::find(sources_, coord, &ChunkSource::coord);
        if (source != sources_.end()) requested_.insert(entry, *source);
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
            if (!inserted && pending_origin_ == origin_) continue;
            const auto matrices = resolve_project_matrices(project);
            const auto placement = offset(chunk.coord, pending_origin_);
            for (std::size_t i = 0; i < project.nodes.size(); ++i) {
                auto matrix = matrices[i];
                matrix[12] += placement.x;
                matrix[13] += placement.y;
                const auto entity = inserted ? candidate.create() : it->second[i];
                if (!candidate.set_render_pose(entity, matrix, project.nodes[i].mesh)) {
                    throw std::invalid_argument("Invalid streamed chunk render transform");
                }
                if (inserted) it->second.push_back(entity);
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

    [[nodiscard]] ChunkCollision candidate_geometry(std::span<const ChunkSource> requested) const {
        ChunkCollision geometry;
        for (const auto& chunk : requested) {
            const auto placement = offset(chunk.coord, pending_origin_);
            const float x = placement.x, y = placement.y;
            const auto to_local = [&](Vec3 point) {
                if (point.z > max_ground_height) throw std::invalid_argument("Course collision exceeds the supported gameplay height");
                if (chunk_local_collision_) {
                    point.x += x;
                    point.y += y;
                }
                return point;
            };
            const auto boxes = stream_.staged_collision(chunk.coord).value();
            for (const auto& box : boxes) {
                geometry.boxes.push_back({to_local(box.min), to_local(box.max)});
            }
            const auto triangles = stream_.staged_triangles(chunk.coord).value();
            for (const auto& triangle : triangles) {
                geometry.triangles.push_back({to_local(triangle.a), to_local(triangle.b), to_local(triangle.c)});
            }

            // Guard the resident perimeter. A delayed or failed load cannot strand the player in a missing chunk.
            constexpr float edge = 0.125f;
            constexpr std::array neighbors{ChunkCoord{-1, 0}, ChunkCoord{1, 0}, ChunkCoord{0, -1}, ChunkCoord{0, 1}};
            const std::array guards{
                Aabb{{x - edge, y, -local_extent}, {x, y + cell_size, local_extent}},
                Aabb{{x + cell_size, y, -local_extent}, {x + cell_size + edge, y + cell_size, local_extent}},
                Aabb{{x, y - edge, -local_extent}, {x + cell_size, y, local_extent}},
                Aabb{{x, y + cell_size, -local_extent}, {x + cell_size, y + cell_size + edge, local_extent}},
            };
            for (std::size_t side = 0; side < neighbors.size(); ++side) {
                const auto neighbor_x = static_cast<std::int64_t>(chunk.coord.x) + neighbors[side].x;
                const auto neighbor_y = static_cast<std::int64_t>(chunk.coord.y) + neighbors[side].y;
                if (std::ranges::none_of(requested, [&](const auto& source) {
                    return source.coord.x == neighbor_x && source.coord.y == neighbor_y;
                })) {
                    geometry.boxes.push_back(guards[side]);
                }
            }
        }
        return geometry;
    }

    static constexpr float cell_size = 8;
    static constexpr float local_extent = 256;
    static constexpr float max_ground_height = 240; // Leave room for capsule height and jumping.
    IDesktopPlatform& platform_;
    std::vector<ChunkSource> sources_;
    CourseSceneOutput* output_;
    ChunkEntities chunk_entities_;
    ChunkCoord origin_{};
    ChunkCoord pending_origin_{};
    ChunkCoord checkpoint_cell_{};
    std::uint64_t origin_epoch_{};
    ChunkStream stream_{{11}};
    std::optional<DesktopGame> game_;
    std::optional<ChunkCoord> requested_center_;
    std::optional<ChunkCoord> requested_vehicle_cell_;
    std::vector<ChunkSource> requested_;
    std::vector<ChunkCoord> resident_coords_;
    std::vector<ChunkCoord> pending_coords_;
    bool reset_held_{};
    bool chunk_local_collision_{};
    bool background_{};
};

} // namespace saige::sample
