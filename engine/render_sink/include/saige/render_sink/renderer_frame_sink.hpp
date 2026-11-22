// SAIGE graphics adapter: implements Core's saige::IFrameSink with the RHI scene renderer.
//
// Contract: docs/core-rendering-contract.md (D-08, D-12, D-15, D-18) and include/saige/runtime.hpp.
// - submit() runs synchronously on the host thread. FrameView instances are copied before returning.
// - Windowed mode reads IDesktopPlatform::surface() on every submit: a changed revision resizes the swapchain;
//   zero extent or minimized skips the frame (no acquire/present).
// - Destruction waits for the GPU and releases the swapchain and surface. Declare the sink after the platform
//   so it is destroyed first (the platform destroys the HWND).
#pragma once

#include "saige/asset_id.hpp"
#include "saige/desktop.hpp"
#include "saige/gfx_scene/stage_model.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/rhi/device.hpp"
#include "saige/runtime.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace saige::render {

struct FrameSinkSettings {
    RendererSettings renderer;
    bool vsync = true;
};

struct FrameSinkStats {
    uint64_t frames_submitted = 0;
    uint64_t frames_rendered = 0;
    uint64_t frames_skipped = 0;          ///< minimized, zero extent or swapchain not ready
    uint64_t swapchain_recreations = 0;
    std::size_t last_instance_count = 0;
    uint64_t model_instances_drawn = 0;     ///< imported-model placements drawn, summed over frames
    uint64_t model_instances_skipped = 0;   ///< non-finite or degenerate world matrices (the renderer skips them)
    uint64_t unresolved_instances = 0;      ///< instances naming an asset that is not staged: drawn as a fallback cube
};

/// What the renderer can report about itself, versioned for the app's diagnostics output. Every value that the device cannot
/// measure is absent (`null` in JSON), never zero: a software rasterizer's timestamps measure the CPU, and a device
/// without timestamp support reports none.
struct RendererDiagnostics {
    static constexpr int schema_version = 1;
    std::string adapter;                       ///< device name, so a software adapter is identified by name
    bool software_device = false;              ///< CPU implementation (e.g. lavapipe): timings are not GPU timings
    bool gpu_timestamps_supported = false;
    std::optional<double> gpu_scene_pass_ms;   ///< scene pass duration of the latest completed, collected frame
    uint64_t gpu_timing_frame = 0;             ///< which frame that is (valid only with gpu_scene_pass_ms)
    rhi::DeviceMemoryStats device_memory;      ///< backend allocations (exact) and, if reported, the driver's budget
    ResourceStats resources;                   ///< registry totals: live resources and their estimated bytes
    uint32_t validation_errors = 0;
};

/// Stable JSON for `RendererDiagnostics`: `{"schema_version":1,...}` with `null` for anything unavailable.
[[nodiscard]] std::string to_json(const RendererDiagnostics& diagnostics);

/// One asset to stage: its stable ID (Core's `RenderInstance::mesh`) and its imported CPU data. The data is only read
/// during `stage_scene`.
struct SceneAssetRef {
    saige::AssetId id;
    const gfx::GltfAsset* asset = nullptr;
    std::string name;  ///< prefixes the GPU debug names so a leak report points at the asset
};

class RendererFrameSink;

/// A complete set of GPU models, staged but not yet shown. Move-only. Dropping it (for example after a failed staging
/// elsewhere, or a replaced scene) releases its GPU resources through the registry; the device retires them when the GPU
/// is done with any frame still using them. It must not outlive the sink that created it.
class StagedScene {
public:
    StagedScene() = default;
    StagedScene(StagedScene&&) noexcept = default;
    StagedScene& operator=(StagedScene&&) noexcept = default;
    StagedScene(const StagedScene&) = delete;
    StagedScene& operator=(const StagedScene&) = delete;

    [[nodiscard]] std::size_t model_count() const noexcept { return models_.size(); }
    [[nodiscard]] bool empty() const noexcept { return models_.empty(); }
    /// Totals over every staged asset, with the warnings of all of them.
    [[nodiscard]] const gfx::StageReport& report() const noexcept { return report_; }
    [[nodiscard]] const GpuModel* find(saige::AssetId id) const noexcept;

private:
    friend class RendererFrameSink;
    struct Entry {
        saige::AssetId id;
        GpuModel model;
    };
    std::vector<Entry> models_;  ///< sorted by id
    gfx::StageReport report_;
    const RendererFrameSink* owner_ = nullptr;  ///< the sink whose registry owns the GPU resources (activation checks it)
};

/// Converts Core's frame snapshot into renderer instances (positions in world meters, stable per-entity colors).
[[nodiscard]] std::vector<SceneInstance> to_scene_instances(std::span<const saige::RenderInstance> instances);

/// Core's camera pose (XYZW quaternion, local +X forward, +Z up) as the renderer's camera. Same convention, no change of basis.
[[nodiscard]] Camera to_render_camera(const saige::CameraView& camera) noexcept;

/// Core's authoritative world matrix (column-major, translation at indices 12-14) as the renderer's matrix.
[[nodiscard]] Mat4 to_render_matrix(const saige::WorldMatrix& matrix) noexcept;

class RendererFrameSink final : public saige::IFrameSink {
public:
    /// Windowed: renders to the platform's native window. Throws if the window system is headless or the device
    /// cannot present.
    RendererFrameSink(std::unique_ptr<rhi::Device> device, const saige::IDesktopPlatform& platform,
                      FrameSinkSettings settings = {});
    /// Offscreen: renders into an internal RGBA8 sRGB target and reads it back (headless runs, tests, captures).
    RendererFrameSink(std::unique_ptr<rhi::Device> device, uint32_t width, uint32_t height,
                      FrameSinkSettings settings = {});
    ~RendererFrameSink() override;
    RendererFrameSink(const RendererFrameSink&) = delete;
    RendererFrameSink& operator=(const RendererFrameSink&) = delete;

    void submit(const saige::FrameView& frame) override;

    /// Stages every asset on the GPU (decoding images, uploading meshes and textures, building materials) and returns the
    /// candidate without showing it. All-or-nothing: if any asset fails, this throws std::runtime_error naming the asset
    /// and image or mesh, nothing is retained, and the currently active scene keeps drawing untouched. Duplicate asset IDs
    /// or the built-in (all-zero) ID throw std::invalid_argument. Call between submits (it uploads synchronously).
    [[nodiscard]] StagedScene stage_scene(std::span<const SceneAssetRef> assets, const gfx::StageSettings& settings = {});
    /// Makes a staged scene the active one. A scene staged by a different sink throws std::invalid_argument (its resources
    /// belong to another device); a default-constructed (empty) scene is allowed and clears the active scene. The previous scene's references are released here; frames still in flight keep
    /// their resources until the device retires them. An empty scene is allowed (instances then fall back to cubes).
    void activate_scene(StagedScene&& scene);
    [[nodiscard]] std::size_t active_model_count() const noexcept { return active_.model_count(); }
    [[nodiscard]] SceneRenderer& renderer_mutable() noexcept { return *renderer_; }
    /// Snapshot of timing and memory. GPU timing lags the latest submit by up to `frames_in_flight` frames and is absent
    /// until a frame has completed on a device that supports timestamps.
    [[nodiscard]] RendererDiagnostics diagnostics() const;

    [[nodiscard]] const FrameSinkStats& stats() const noexcept { return stats_; }
    [[nodiscard]] rhi::Device& device() noexcept { return *device_; }
    [[nodiscard]] const SceneRenderer& renderer() const noexcept { return *renderer_; }

    /// Offscreen mode: waits for the GPU and returns the last rendered frame as tightly packed RGBA8 (sRGB).
    /// Empty in windowed mode or before the first frame.
    [[nodiscard]] std::vector<uint8_t> read_back_rgba();
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }

private:
    void submit_windowed(const SceneFrame& frame);
    void submit_offscreen(const SceneFrame& frame);
    void sync_surface();

    std::unique_ptr<rhi::Device> device_;
    const saige::IDesktopPlatform* platform_ = nullptr;
    FrameSinkSettings settings_;
    std::unique_ptr<SceneRenderer> renderer_;
    std::vector<SceneInstance> instances_;  // copy of the current FrameView (contract: copy what you retain)
    std::vector<ModelDraw> model_draws_;    // imported-model draws of the current FrameView
    Camera camera_;                         // the current FrameView's camera pose, if it has one
    StagedScene active_;
    FrameSinkStats stats_;

    // Windowed
    rhi::SwapchainHandle swapchain_;
    uint64_t surface_revision_ = 0;
    bool surface_drawable_ = false;
    bool recreate_pending_ = false;

    // Offscreen
    rhi::TextureHandle offscreen_color_;
    /// What the previous offscreen frame left the target in (its readback copy), so the next frame's first barrier waits for
    /// that copy instead of discarding the image underneath it. Undefined before the first frame.
    rhi::ResourceState offscreen_state_;
    rhi::BufferHandle readback_;
    bool has_frame_ = false;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace saige::render
