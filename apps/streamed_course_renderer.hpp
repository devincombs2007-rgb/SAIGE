#pragma once

#include "streamed_course.hpp"
#include "saige/render_sink/renderer_frame_sink.hpp"

namespace saige::sample {

class StreamedCourseRenderer final : public CourseSceneOutput {
public:
    explicit StreamedCourseRenderer(render::RendererFrameSink& sink) : sink_(sink) {}

    bool stage(std::span<const CourseAsset> assets) override {
        bool uploaded = false;
        std::vector<const render::StagedScene*> parts;
        parts.reserve(assets.size());
        for (const auto& asset : assets) {
            const auto key = std::pair{asset.id.high, asset.id.low};
            auto next = pending_.find(key);
            if (next == pending_.end()) {
                const auto retained = active_.find(key);
                if (retained != active_.end() && retained->second.source == asset.source) {
                    next = pending_.emplace(key, retained->second).first;
                } else {
                    const auto frame = sink_.stats().frames_submitted;
                    if (uploaded || (live_ && upload_frame_ == frame)) {
                        ++deferred_uploads_;
                        return false;
                    }
                    const render::SceneAssetRef ref{asset.id, asset.model, format_asset_id(asset.id)};
                    const auto start = std::chrono::steady_clock::now();
                    auto staged = sink_.stage_scene(std::span(&ref, 1));
                    upload_time_ += std::chrono::steady_clock::now() - start;
                    ++uploads_;
                    upload_frame_ = frame;
                    auto scene = std::make_shared<const render::StagedScene>(std::move(staged));
                    next = pending_.emplace(key, ResidentAsset{asset.source, std::move(scene)}).first;
                    uploaded = true;
                }
            }
            parts.push_back(next->second.scene.get());
        }
        staged_ = sink_.compose_staged_scenes(parts);
        return true;
    }

    void publish() noexcept override {
        // The same sink staged this candidate, so the ownership check cannot fail.
        sink_.activate_scene(std::move(staged_));
        active_.swap(pending_);
        pending_.clear();
        if (!live_) upload_frame_.reset(); // Bootstrap uploads do not consume the first live frame budget.
        live_ = true;
    }

    void discard() noexcept override {
        staged_ = {};
        pending_.clear();
    }

    [[nodiscard]] std::uint64_t uploads() const noexcept { return uploads_; }
    [[nodiscard]] std::uint64_t deferred_uploads() const noexcept { return deferred_uploads_; }
    [[nodiscard]] auto upload_time() const noexcept { return upload_time_; }

private:
    struct ResidentAsset {
        std::filesystem::path source;
        std::shared_ptr<const render::StagedScene> scene;
    };
    render::RendererFrameSink& sink_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, ResidentAsset> active_;
    std::map<std::pair<std::uint64_t, std::uint64_t>, ResidentAsset> pending_;
    render::StagedScene staged_;
    std::uint64_t uploads_{};
    std::uint64_t deferred_uploads_{};
    std::optional<std::uint64_t> upload_frame_;
    bool live_{};
    std::chrono::steady_clock::duration upload_time_{};
};

} // namespace saige::sample
