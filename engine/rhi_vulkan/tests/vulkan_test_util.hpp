// Shared helpers for the Vulkan scene tests: an offscreen target that renders a scene and returns an Image.
#pragma once

#include "saige/gfx_test/image.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <cstdlib>
#include <memory>
#include <span>
#include <vector>

namespace saige::test {

/// `synchronization`: also enable the layer's synchronization (hazard) validation. Check
/// `device->info().synchronization_validation_enabled`; it is unavailable without VK_EXT_validation_features.
inline std::unique_ptr<rhi::Device> make_validated_device(const char* name, bool synchronization = false)
{
    rhi::DeviceDesc desc;
    desc.enable_validation = true;
    desc.enable_synchronization_validation = synchronization;
    const char* require = std::getenv("SAIGE_REQUIRE_VALIDATION");  // CI: a missing validation layer is a failure
    desc.require_validation = require != nullptr && require[0] != '\0' && require[0] != '0';
    desc.application_name = name;
    return rhi::create_vulkan_device(desc);
}

/// An offscreen RGBA8 sRGB target plus readback buffer. Destroy it before the device (it releases its resources).
class OffscreenTarget {
public:
    OffscreenTarget(rhi::Device& device, uint32_t width, uint32_t height) : device_(device), width_(width), height_(height)
    {
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_SRGB;
        td.width = width;
        td.height = height;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        td.debug_name = "SceneTestTarget";
        color_ = device.create_texture(td);
        rhi::BufferDesc bd;
        bd.size = uint64_t{width} * height * 4;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        bd.debug_name = "SceneTestReadback";
        readback_ = device.create_buffer(bd);
    }
    ~OffscreenTarget()
    {
        device_.destroy(readback_);
        device_.destroy(color_);
    }
    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    /// Renders one frame and returns it. Animation is derived from `tick`, so a fixed tick is deterministic.
    Image render(render::SceneRenderer& renderer, const std::vector<render::SceneInstance>& instances,
                 const render::Camera& camera, uint64_t tick = 0, std::span<const render::ModelDraw> models = {},
                 render::FrameStats* stats_out = nullptr)
    {
        render::SceneFrame frame;
        frame.tick = tick;
        frame.instances = instances;
        frame.camera = &camera;
        frame.models = models;
        render::RenderTarget target;
        target.texture = color_;
        target.width = width_;
        target.height = height_;
        target.readback_buffer = readback_;
        rhi::CommandList& cmd = device_.begin_frame();
        const render::FrameStats stats = renderer.render(cmd, frame, target);
        if (stats_out != nullptr) {
            *stats_out = stats;
        }
        (void)device_.end_frame();
        device_.wait_idle();
        const auto bytes = device_.map(readback_);
        Image img;
        img.width = width_;
        img.height = height_;
        img.rgba.assign(reinterpret_cast<const uint8_t*>(bytes.data()),
                        reinterpret_cast<const uint8_t*>(bytes.data()) + size_t{width_} * height_ * 4);
        return img;
    }

    [[nodiscard]] rhi::TextureHandle color_texture() const noexcept { return color_; }
    [[nodiscard]] uint32_t width() const noexcept { return width_; }
    [[nodiscard]] uint32_t height() const noexcept { return height_; }

private:
    rhi::Device& device_;
    uint32_t width_;
    uint32_t height_;
    rhi::TextureHandle color_;
    rhi::BufferHandle readback_;
};

/// Renderer settings for deterministic scenes: no spin, so a cube's yaw depends only on its own yaw_offset.
inline render::RendererSettings deterministic_settings()
{
    render::RendererSettings s;
    s.spin_radians_per_second = 0.0f;
    return s;
}

struct Mask {
    uint32_t count = 0;
    double cx = 0.0;
    double cy = 0.0;
};

/// Pixels that differ between two renders by more than `threshold` in any channel, and their centroid.
inline Mask changed_pixels(const Image& a, const Image& b, int threshold = 24)
{
    Mask m;
    for (uint32_t y = 0; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) {
            int delta = 0;
            for (int c = 0; c < 3; ++c) {
                delta = std::max(delta, std::abs(int{a.at(x, y)[c]} - int{b.at(x, y)[c]}));
            }
            if (delta > threshold) {
                ++m.count;
                m.cx += x + 0.5;
                m.cy += y + 0.5;
            }
        }
    }
    if (m.count > 0) {
        m.cx /= m.count;
        m.cy /= m.count;
    }
    return m;
}

inline int max_channel_delta(const uint8_t* a, const uint8_t* b)
{
    int delta = 0;
    for (int c = 0; c < 3; ++c) {
        delta = std::max(delta, std::abs(int{a[c]} - int{b[c]}));
    }
    return delta;
}

/// Pixels where `alone` (a render containing only some objects) differs from `empty`: those objects' silhouette.
inline uint32_t silhouette_pixels(const Image& alone, const Image& empty, int threshold = 24)
{
    uint32_t n = 0;
    for (uint32_t y = 0; y < alone.height; ++y) {
        for (uint32_t x = 0; x < alone.width; ++x) {
            n += max_channel_delta(alone.at(x, y), empty.at(x, y)) > threshold ? 1u : 0u;
        }
    }
    return n;
}

/// How many of the objects' pixels in `alone` are still visible, unchanged, in `combined`. Independent of colors, so
/// it cannot be fooled by a bluish sky or ground the way a hue test can.
inline uint32_t visible_pixels(const Image& combined, const Image& alone, const Image& empty, int threshold = 24)
{
    uint32_t n = 0;
    for (uint32_t y = 0; y < alone.height; ++y) {
        for (uint32_t x = 0; x < alone.width; ++x) {
            const bool belongs = max_channel_delta(alone.at(x, y), empty.at(x, y)) > threshold;
            n += belongs && max_channel_delta(combined.at(x, y), alone.at(x, y)) <= 2 ? 1u : 0u;
        }
    }
    return n;
}

} // namespace saige::test
