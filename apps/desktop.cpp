#include "saige/win32_platform.hpp"

#if defined(SAIGE_DESKTOP_VULKAN)
#include "saige/render_sink/renderer_frame_sink.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <memory>
#endif
#if defined(SAIGE_DESKTOP_SCENE)
#include "saige/prepared_scene.hpp"
#include "saige/render_sink/prepared_scene.hpp"

#include <filesystem>
#endif

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
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
    DesktopFrameSink(DesktopFrameProbe& probe, const saige::IDesktopPlatform& platform,
                     bool require_rendered, bool validate) : probe_(probe) {
        try {
            saige::rhi::DeviceDesc desc;
            desc.enable_validation = validate;
            desc.require_validation = validate;
            desc.require_presentation = true;
            renderer_ = std::make_unique<saige::render::RendererFrameSink>(saige::rhi::create_vulkan_device(desc),
                                                                           platform);
            std::clog << "SAIGE desktop renderer: Vulkan on " << renderer_->device().info().device_name << '\n';
        } catch (const std::exception& error) {
            if (require_rendered) throw;
            std::cerr << "SAIGE desktop renderer unavailable, running without graphics: " << error.what() << '\n';
        }
    }

    void submit(const saige::FrameView& frame) override {
        probe_.submit(frame);
        if (renderer_) {
            renderer_->submit(frame);
        }
    }

#if defined(SAIGE_DESKTOP_SCENE)
    [[nodiscard]] bool has_renderer() const noexcept { return renderer_ != nullptr; }

    // Stages the prepared scene's GPU resources and makes them active. Staging is all-or-nothing and the renderer keeps its
    // previous scene (none, at startup) if it throws; the exception propagates to main's error report.
    void show(const saige::PreparedScene& scene) {
        if (!renderer_) {
            return;
        }
        renderer_->activate_scene(saige::render::stage_prepared_scene(*renderer_, scene));
    }

    [[nodiscard]] std::string diagnostics_json() const {
        return renderer_ ? saige::render::to_json(renderer_->diagnostics()) : std::string{};
    }
#endif

    [[nodiscard]] std::uint32_t validation_errors() {
        if (!renderer_) {
            return 0;
        }
        renderer_->device().wait_idle();
        return renderer_->device().validation_error_count();
    }

    [[nodiscard]] std::uint64_t rendered() const {
        return renderer_ ? renderer_->stats().frames_rendered : 0;
    }

private:
    DesktopFrameProbe& probe_;
    std::unique_ptr<saige::render::RendererFrameSink> renderer_;
};
#endif

} // namespace

int main(int argc, char** argv) {
    std::optional<std::uint64_t> frames;
#if defined(SAIGE_DESKTOP_VULKAN)
    bool require_rendered = false;
    bool validate = false;
#endif
#if defined(SAIGE_DESKTOP_SCENE)
    std::optional<std::filesystem::path> scene_path;
    bool skip_without_gpu = false;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
#if defined(SAIGE_DESKTOP_VULKAN)
        if (argument == "--require-rendered" && !require_rendered) {
            require_rendered = true;
            continue;
        }
        if (argument == "--validate" && !validate) {
            validate = true;
            continue;
        }
#endif
#if defined(SAIGE_DESKTOP_SCENE)
        if (argument == "--scene" && !scene_path && i + 1 < argc) {
            scene_path = std::filesystem::path(argv[++i]);
            continue;
        }
        if (argument == "--skip-without-gpu" && !skip_without_gpu) {
            skip_without_gpu = true;  // tests: exit 77 (ctest SKIP_RETURN_CODE) instead of running the probe-only fallback
            continue;
        }
#endif
        if (argument != "--frames" || frames || ++i == argc) {
            std::cerr << "Usage: saige_desktop [--frames COUNT]"
#if defined(SAIGE_DESKTOP_VULKAN)
                      << " [--require-rendered] [--validate]"
#endif
#if defined(SAIGE_DESKTOP_SCENE)
                      << " [--scene PROJECT.json] [--skip-without-gpu]"
#endif
                      << '\n';
            return 2;
        }
        const std::string_view value(argv[i]);
        std::uint64_t count{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
        if (error != std::errc{} || end != value.data() + value.size()) {
            std::cerr << "--frames expects a nonnegative integer\n";
            return 2;
        }
        frames = count;
    }

    try {
        constexpr auto title = L"SAIGE | WASD to move | Esc to quit";
#if defined(SAIGE_DESKTOP_VULKAN)
        const bool strict = require_rendered || validate;
        saige::Win32Platform platform(title, 960, 540, strict ? saige::DesktopPresentation::renderer_paced
                                                            : saige::DesktopPresentation::host_paced);
#else
        saige::Win32Platform platform(title);
#endif
        saige::World world;
        DesktopGame game(world, platform);
        DesktopFrameProbe probe;
#if defined(SAIGE_DESKTOP_SCENE)
        // CPU preparation first: a bad project or asset fails here, before any window or GPU work is spent on it.
        std::optional<saige::PreparedScene> prepared;
        if (scene_path) {
            prepared.emplace(*scene_path);
        }
#endif
#if defined(SAIGE_DESKTOP_VULKAN)
        // Declared after the platform: the renderer releases its swapchain and surface before the HWND goes away.
#if defined(SAIGE_DESKTOP_SCENE)
        // With --skip-without-gpu a missing device is reported as "skipped" below instead of failing the sink's construction.
        DesktopFrameSink sink(probe, platform, strict && !skip_without_gpu, validate);
#else
        DesktopFrameSink sink(probe, platform, strict, validate);
#endif
#if defined(SAIGE_DESKTOP_SCENE)
        if (prepared) {
            if (!sink.has_renderer() && skip_without_gpu) {
                std::cout << "SKIPPED: no usable Vulkan device for the scene run\n";
                return 77;
            }
            sink.show(*prepared);
        }
        const auto stats = prepared ? prepared->run(platform, sink, frames) : saige::run(world, platform, game, sink, frames);
        if (prepared && sink.has_renderer()) {
            std::cout << "SAIGE scene diagnostics " << sink.diagnostics_json() << '\n';
        }
#else
        const auto stats = saige::run(world, platform, game, sink, frames);
#endif
        std::cout << "SAIGE Vulkan rendered=" << sink.rendered() << '\n';
        if (sink.validation_errors() != 0) {
            std::cerr << "SAIGE desktop renderer: " << sink.validation_errors() << " Vulkan validation error(s)\n";
            return 1;
        }
        if (strict && sink.rendered() == 0) {
            std::cerr << "SAIGE Vulkan did not render a frame\n";
            return 1;
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
