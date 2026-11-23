#include "saige/win32_platform.hpp"
#include "desktop_game.hpp"

#if defined(SAIGE_DESKTOP_VULKAN)
#include "saige/render_sink/renderer_frame_sink.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <memory>
#endif
#if defined(SAIGE_DESKTOP_SCENE)
#include "saige/prepared_scene.hpp"
#include "saige/render_sink/prepared_scene.hpp"
#endif

#if defined(SAIGE_DESKTOP_COURSE_FILES)
#include "saige/chunk_manifest.hpp"

#include <vector>
#endif

#if defined(SAIGE_DESKTOP_SCENE) || defined(SAIGE_DESKTOP_COURSE_FILES)
#include <filesystem>
#endif

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

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
                     bool require_rendered, bool validate, bool course) : probe_(probe) {
        try {
            saige::rhi::DeviceDesc desc;
            desc.enable_validation = validate;
            desc.require_validation = validate;
            desc.require_presentation = true;
            saige::render::FrameSinkSettings settings;
            if (course) settings.renderer.spin_radians_per_second = 0.0f;
            renderer_ = std::make_unique<saige::render::RendererFrameSink>(saige::rhi::create_vulkan_device(desc),
                                                                           platform, settings);
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
    // Stages the prepared scene's GPU resources and makes them active. Staging is all-or-nothing and the renderer keeps its
    // previous scene (none, at startup) if it throws; the exception propagates to main's error report.
    void show(const saige::PreparedScene& scene) {
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
    bool course = false;
#if defined(SAIGE_DESKTOP_COURSE_FILES)
    std::optional<std::filesystem::path> course_path;
#endif
#if defined(SAIGE_DESKTOP_VULKAN)
    bool require_rendered = false;
    bool validate = false;
#endif
#if defined(SAIGE_DESKTOP_SCENE)
    std::optional<std::filesystem::path> scene_path;
#endif
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--character-course" && !course) {
            course = true;
            continue;
        }
#if defined(SAIGE_DESKTOP_COURSE_FILES)
        if (argument == "--course-collision" && !course_path && i + 1 < argc) {
            course_path = std::filesystem::path(argv[++i]);
            continue;
        }
#endif
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
#endif
        if (argument != "--frames" || frames || ++i == argc) {
            std::cerr << "Usage: saige_desktop [--frames COUNT] [--character-course]"
#if defined(SAIGE_DESKTOP_COURSE_FILES)
                      << " [--course-collision FILE.json]"
#endif
#if defined(SAIGE_DESKTOP_VULKAN)
                      << " [--require-rendered] [--validate]"
#endif
#if defined(SAIGE_DESKTOP_SCENE)
                      << " [--scene PROJECT.json]"
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
        std::optional<std::span<const saige::Aabb>> course_boxes;
#if defined(SAIGE_DESKTOP_COURSE_FILES)
        std::vector<saige::Aabb> collision;
        if (course_path) {
            if (!course) {
                std::cerr << "--course-collision requires --character-course\n";
                return 2;
            }
            collision = saige::load_chunk_collision(*course_path);
            course_boxes = std::span<const saige::Aabb>(collision);
        }
#endif
#if defined(SAIGE_DESKTOP_SCENE)
        // Validate the saved project and its CPU assets before opening a window.
        std::optional<saige::PreparedScene> prepared;
        if (scene_path) {
            if (course) {
                std::cerr << "--character-course and --scene select different applications\n";
                return 2;
            }
            prepared.emplace(*scene_path);
            require_rendered = true;
        }
#endif
#if defined(SAIGE_DESKTOP_VULKAN)
        const auto title = course ? L"SAIGE course | WASD | Shift sprint | Space jump | Right drag camera | R reset"
                                  : L"SAIGE | WASD to move | Right drag to look | Esc to quit";
        const bool strict = require_rendered || validate;
        saige::Win32Platform platform(title, 960, 540, strict ? saige::DesktopPresentation::renderer_paced
                                                            : saige::DesktopPresentation::host_paced);
#else
        saige::Win32Platform platform(L"SAIGE | WASD to move | Esc to quit");
#endif
        saige::World world;
        saige::sample::DesktopGame game(world, platform, course, course_boxes);
        DesktopFrameProbe probe;
#if defined(SAIGE_DESKTOP_VULKAN)
        // The sink releases its swapchain and surface before the HWND goes away.
        DesktopFrameSink sink(probe, platform, strict, validate, course);
#if defined(SAIGE_DESKTOP_SCENE)
        if (prepared) {
            sink.show(*prepared);
        }
        const auto stats = prepared ? prepared->run(platform, sink, frames) : saige::run(world, platform, game, sink, frames);
        if (prepared) {
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
