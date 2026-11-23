#pragma once

#include "streamed_course.hpp"
#include "saige/render_sink/renderer_frame_sink.hpp"

namespace saige::sample {

class StreamedCourseRenderer final : public CourseSceneOutput {
public:
    explicit StreamedCourseRenderer(render::RendererFrameSink& sink) : sink_(sink) {}

    void stage(std::span<const CourseAsset> assets) override {
        std::vector<render::SceneAssetRef> refs;
        refs.reserve(assets.size());
        for (const auto& asset : assets) {
            refs.push_back({asset.id, asset.model, format_asset_id(asset.id)});
        }
        staged_ = sink_.stage_scene(refs);
    }

    void publish() noexcept override {
        // The same sink staged this candidate, so the ownership check cannot fail.
        sink_.activate_scene(std::move(staged_));
    }

    void discard() noexcept override { staged_ = {}; }

private:
    render::RendererFrameSink& sink_;
    render::StagedScene staged_;
};

} // namespace saige::sample
