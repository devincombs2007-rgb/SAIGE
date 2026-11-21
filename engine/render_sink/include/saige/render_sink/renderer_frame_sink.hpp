// SAIGE graphics adapter: implements Core's saige::IFrameSink with the RHI scene renderer.
//
// Contract: docs/core-rendering-contract.md (D-08, D-12, D-15, D-18) and include/saige/runtime.hpp.
// - submit() runs synchronously on the host thread. FrameView instances are copied before returning.
// - Windowed mode reads IDesktopPlatform::surface() on every submit: a changed revision resizes the swapchain;
//   zero extent or minimized skips the frame (no acquire/present).
// - Destruction waits for the GPU and releases the swapchain and surface. Declare the sink after the platform
//   so it is destroyed first (the platform destroys the HWND).
#pragma once

#include "saige/desktop.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/rhi/device.hpp"
#include "saige/runtime.hpp"

#include <cstdint>
#include <memory>
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
};

/// Converts Core's frame snapshot into renderer instances (positions in world meters, stable per-entity colors).
[[nodiscard]] std::vector<SceneInstance> to_scene_instances(std::span<const saige::RenderInstance> instances);

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
    FrameSinkStats stats_;

    // Windowed
    rhi::SwapchainHandle swapchain_;
    uint64_t surface_revision_ = 0;
    bool surface_drawable_ = false;
    bool recreate_pending_ = false;

    // Offscreen
    rhi::TextureHandle offscreen_color_;
    rhi::BufferHandle readback_;
    bool has_frame_ = false;

    uint32_t width_ = 0;
    uint32_t height_ = 0;
};

} // namespace saige::render
