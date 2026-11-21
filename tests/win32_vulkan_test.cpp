#include "saige/render_sink/renderer_frame_sink.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"
#include "saige/win32_platform.hpp"

#include <windows.h>

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool result, const char* message) {
    if (!result) {
        throw std::runtime_error(message);
    }
}

} // namespace

int main() {
    try {
        saige::Win32Platform platform(L"SAIGE Vulkan acceptance", 420, 300);
        const auto window = reinterpret_cast<HWND>(platform.native_window().window);
        require(IsWindow(window) != FALSE, "Win32 did not create a window");

        {
            saige::rhi::DeviceDesc desc;
            desc.require_presentation = true;
            desc.enable_validation = true;
            desc.require_validation = true;
            saige::render::RendererFrameSink sink(saige::rhi::create_vulkan_device(desc), platform);
            const auto& info = sink.device().info();
            std::cout << "Vulkan device: " << info.device_name
                      << " software=" << std::boolalpha << info.software_device
                      << " validation=" << info.validation_enabled << '\n';
            require(info.validation_enabled, "Vulkan validation is required");

            std::array<saige::RenderInstance, 1> instances{{{{0, 1}, {0, 0, 0}}}};
            std::uint64_t tick = 0;
            const auto submit = [&] {
                require(platform.poll_events(), "Window quit before submission");
                sink.submit({++tick, 0.0, instances});
            };
            const auto render = [&] {
                const auto before = sink.stats().frames_rendered;
                for (int attempt = 0; attempt < 8 && sink.stats().frames_rendered == before; ++attempt) {
                    submit();
                }
                require(sink.stats().frames_rendered > before, "Vulkan did not render a drawable frame");
            };

            render();
            const auto initial_revision = platform.surface().revision;
            const auto initial_recreations = sink.stats().swapchain_recreations;
            require(SetWindowPos(window, nullptr, 0, 0, 590, 410,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
                    "Could not resize the window");
            render();
            require(platform.surface().revision > initial_revision &&
                        sink.stats().swapchain_recreations > initial_recreations,
                    "Resize did not recreate the swapchain");

            ShowWindow(window, SW_SHOWMINNOACTIVE);
            const auto before_minimize = sink.stats();
            submit();
            require(platform.surface().minimized &&
                        sink.stats().frames_skipped > before_minimize.frames_skipped &&
                        sink.stats().frames_rendered == before_minimize.frames_rendered,
                    "Minimized window did not skip rendering");
            ShowWindow(window, SW_RESTORE);
            render();

            require(PostMessageW(window, WM_KEYDOWN, 'W', 0) != FALSE, "Could not queue keyboard input");
            require(platform.poll_events() && platform.key_down(saige::Key::w), "W key down was lost");
            instances[0].position.x = 1;
            render();
            require(PostMessageW(window, WM_KEYUP, 'W', 0) != FALSE, "Could not queue key release");
            require(platform.poll_events() && !platform.key_down(saige::Key::w), "W key stayed held");

            require(PostMessageW(window, WM_CLOSE, 0, 0) != FALSE, "Could not queue close");
            require(!platform.poll_events(), "Window close did not stop the platform");
            sink.device().wait_idle();
            require(sink.device().validation_error_count() == 0, "Vulkan validation reported errors");
            require(IsWindow(window) != FALSE, "Window was destroyed before surface shutdown");
        }
        require(IsWindow(window) != FALSE, "Graphics destroyed the platform window");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
