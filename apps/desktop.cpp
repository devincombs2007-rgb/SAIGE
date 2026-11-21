#include "saige/win32_platform.hpp"

#if defined(SAIGE_DESKTOP_VULKAN)
#include "saige/render_sink/renderer_frame_sink.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <memory>
#endif

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

class DesktopGame final : public saige::IGame {
public:
    DesktopGame(saige::World& world, const saige::IDesktopPlatform& platform)
        : marker_(world.create()), platform_(platform) {}

    void fixed_update(saige::World& world, std::chrono::nanoseconds step) override {
        using saige::Key;
        const int right = platform_.key_down(Key::d) || platform_.key_down(Key::right);
        const int left = platform_.key_down(Key::a) || platform_.key_down(Key::left);
        const int forward = platform_.key_down(Key::w) || platform_.key_down(Key::up);
        const int back = platform_.key_down(Key::s) || platform_.key_down(Key::down);
        const float seconds = std::chrono::duration<float>(step).count();
        // The marker drifts one world unit per second so unattended runs exercise simulation.
        position_.x += seconds * (1.0f + 4.0f * static_cast<float>(forward - back));
        position_.y += seconds * 4.0f * static_cast<float>(left - right);
        world.set_position(marker_, position_);
    }

private:
    saige::Entity marker_;
    const saige::IDesktopPlatform& platform_;
    saige::Vec3 position_{};
};

// The sample reports the submitted pose until a graphics adapter is selected.
class DesktopFrameProbe final : public saige::IFrameSink {
public:
    void submit(const saige::FrameView& frame) override {
        instances = frame.instances.size();
        if (!frame.instances.empty()) {
            position = frame.instances.front().position;
        }
    }

    std::size_t instances{};
    saige::Vec3 position{};
};

#if defined(SAIGE_DESKTOP_VULKAN)
// Feeds each frame to the probe (for the summary line) and, when a GPU is available, to the Vulkan renderer.
class DesktopFrameSink final : public saige::IFrameSink {
public:
    DesktopFrameSink(DesktopFrameProbe& probe, const saige::IDesktopPlatform& platform) : probe_(probe) {
        try {
            saige::rhi::DeviceDesc desc;
#ifndef NDEBUG
            desc.enable_validation = true;
#endif
            desc.require_presentation = true;
            renderer_ = std::make_unique<saige::render::RendererFrameSink>(saige::rhi::create_vulkan_device(desc),
                                                                           platform);
            std::clog << "SAIGE desktop renderer: Vulkan on " << renderer_->device().info().device_name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "SAIGE desktop renderer unavailable, running without graphics: " << error.what() << '\n';
        }
    }

    void submit(const saige::FrameView& frame) override {
        probe_.submit(frame);
        if (renderer_) {
            renderer_->submit(frame);
        }
    }

    [[nodiscard]] std::uint32_t validation_errors() const {
        return renderer_ ? renderer_->device().validation_error_count() : 0;
    }

private:
    DesktopFrameProbe& probe_;
    std::unique_ptr<saige::render::RendererFrameSink> renderer_;
};
#endif

} // namespace

int main(int argc, char** argv) {
    std::optional<std::uint64_t> frames;
    if (argc != 1) {
        if (argc != 3 || std::string_view(argv[1]) != "--frames") {
            std::cerr << "Usage: saige_desktop [--frames COUNT]\n";
            return 2;
        }
        const std::string_view value(argv[2]);
        std::uint64_t count{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
        if (error != std::errc{} || end != value.data() + value.size()) {
            std::cerr << "--frames expects a nonnegative integer\n";
            return 2;
        }
        frames = count;
    }

    try {
#if defined(SAIGE_DESKTOP_VULKAN)
        // A presenting renderer paces frames through vsync (Core #12). Without a usable Vulkan device the sample
        // falls back to the console probe, which needs host pacing to avoid spinning.
        const bool gpu_available = saige::rhi::vulkan_device_available();
        saige::Win32Platform platform(L"SAIGE | WASD to move | Esc to quit", 960, 540,
                                      gpu_available ? saige::DesktopPresentation::renderer_paced
                                                    : saige::DesktopPresentation::host_paced);
#else
        saige::Win32Platform platform(L"SAIGE | WASD to move | Esc to quit");
#endif
        saige::World world;
        DesktopGame game(world, platform);
        DesktopFrameProbe probe;
#if defined(SAIGE_DESKTOP_VULKAN)
        // Declared after the platform: the renderer releases its swapchain and surface before the HWND goes away.
        DesktopFrameSink sink(probe, platform);
        const auto stats = saige::run(world, platform, game, sink, frames);
        if (sink.validation_errors() != 0) {
            std::cerr << "SAIGE desktop renderer: " << sink.validation_errors() << " Vulkan validation error(s)\n";
        }
#else
        const auto stats = saige::run(world, platform, game, probe, frames);
#endif
        std::cout << "SAIGE desktop frames=" << stats.frames << " ticks=" << stats.ticks
                  << " instances=" << probe.instances << " marker_x=" << probe.position.x
                  << " marker_y=" << probe.position.y << '\n';
    } catch (const std::exception& error) {
        std::cerr << "SAIGE desktop error: " << error.what() << '\n';
        return 1;
    }
}
