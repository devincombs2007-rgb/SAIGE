// Native-app glue between Core's PreparedScene (CPU assets, stable asset IDs) and the renderer's two-phase scene swap.
//
//   saige::PreparedScene scene(path);                                  // CPU only; throws on a bad project or asset
//   saige::render::StagedScene gpu = saige::render::stage_prepared_scene(sink, scene);   // GPU only; all-or-nothing
//   sink.activate_scene(std::move(gpu));                               // atomic; the old scene keeps drawing until here
//   scene.run(platform, sink, frames);
//
// Header-only on purpose: it needs Core's headers but no extra link dependency (the app already links saige::scene_project).
// Both steps are transactional, so the order "prepare, stage, activate" means a failure at either of the first two leaves
// the previous scene fully intact. Call between submits: staging uploads synchronously.
#pragma once

#include "saige/prepared_scene.hpp"
#include "saige/render_sink/renderer_frame_sink.hpp"

#include <span>
#include <string>
#include <vector>

namespace saige::render {

/// Stages every imported asset of `scene` on the GPU. GPU debug names are the project's relative asset paths, so a leak
/// report or validation message points at the file. Throws std::runtime_error (naming the asset) if any asset fails;
/// nothing is retained then. `scene` must outlive the call only, not the returned StagedScene.
[[nodiscard]] inline StagedScene stage_prepared_scene(RendererFrameSink& sink, const saige::PreparedScene& scene,
                                                      const gfx::StageSettings& settings = {})
{
    const std::span<const saige::PreparedAsset> assets = scene.assets();
    std::vector<std::string> names;
    names.reserve(assets.size());
    for (const saige::PreparedAsset& asset : assets) {
        std::string name = "asset";
        for (const saige::ProjectAsset& entry : scene.project().assets) {
            if (entry.id == asset.id) {
                name = entry.relative_path;
                break;
            }
        }
        names.push_back(std::move(name));
    }
    std::vector<SceneAssetRef> refs;
    refs.reserve(assets.size());
    for (std::size_t i = 0; i < assets.size(); ++i) {
        refs.push_back({assets[i].id, &assets[i].model, names[i]});
    }
    return sink.stage_scene(refs, settings);
}

} // namespace saige::render
