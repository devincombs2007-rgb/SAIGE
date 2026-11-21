#include "saige/win32_platform.hpp"

#include <windows.h>

#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool result, const char* message) {
    if (!result) {
        throw std::runtime_error(message);
    }
}

void post(HWND window, UINT message, WPARAM key = 0) {
    require(PostMessageW(window, message, key, 0) != FALSE, "Posting a window message failed");
}

} // namespace

int main() {
    try {
        // Compare GetClientRect with the adapter in physical pixels on scaled displays.
        require(SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != nullptr,
                "Could not select the test thread's per-monitor DPI context");
        HWND handle{};
        {
            saige::Win32Platform platform(L"SAIGE lifecycle test", 320, 240);
            const auto native = platform.native_window();
            handle = reinterpret_cast<HWND>(native.window);
            require(native.system == saige::WindowSystem::win32 && native.display != nullptr &&
                        IsWindow(handle),
                    "Desktop platform did not expose a valid native surface");
            ShowWindow(handle, SW_HIDE);
            require(platform.poll_events(), "Window quit during startup");
            const auto initial = platform.surface();
            const UINT window_dpi = GetDpiForWindow(handle);
            require(initial.width_px > 0 && initial.height_px > 0 && window_dpi > 0 &&
                        initial.dpi_scale == static_cast<float>(window_dpi) / 96.0f,
                    "Initial surface had an invalid extent or scale");

            require(SetWindowPos(handle, nullptr, 0, 0, 590, 410,
                                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
                    "SetWindowPos failed");
            require(platform.poll_events(), "Window quit during resize");
            RECT client{};
            require(GetClientRect(handle, &client) != FALSE, "GetClientRect failed");
            const auto resized = platform.surface();
            require(resized.width_px == static_cast<unsigned>(client.right - client.left) &&
                        resized.height_px == static_cast<unsigned>(client.bottom - client.top) &&
                        resized.revision > initial.revision && !resized.minimized,
                    "Surface state did not track the resized client area");

            ShowWindow(handle, SW_SHOWMINNOACTIVE);
            require(platform.poll_events(), "Window quit while minimized");
            const auto minimized = platform.surface();
            require(minimized.minimized && minimized.width_px == 0 && minimized.height_px == 0 &&
                        minimized.revision > resized.revision,
                    "Minimized window still advertised a drawable surface");
            ShowWindow(handle, SW_RESTORE);
            require(platform.poll_events(), "Window quit during restore");
            const auto restored = platform.surface();
            require(!restored.minimized && restored.width_px == resized.width_px &&
                        restored.height_px == resized.height_px && restored.revision > minimized.revision,
                    "Restored window did not recover its drawable surface");
            ShowWindow(handle, SW_HIDE);

            post(handle, WM_KEYDOWN, 'W');
            require(platform.poll_events() && platform.key_down(saige::Key::w),
                    "A queued key press did not reach the input state");
            post(handle, WM_KEYUP, 'W');
            require(platform.poll_events() && !platform.key_down(saige::Key::w),
                    "A queued key release left movement active");

            post(handle, WM_KEYDOWN, VK_LEFT);
            require(platform.poll_events() && platform.key_down(saige::Key::left),
                    "A queued arrow key did not reach the input state");
            post(handle, WM_KILLFOCUS);
            require(platform.poll_events() && !platform.key_down(saige::Key::left),
                    "A held movement key survived focus loss");

            post(handle, WM_CLOSE);
            require(!platform.poll_events() && !platform.poll_events(), "Close did not latch quit");
            require(IsWindow(handle) && platform.native_window().window == native.window,
                    "Close invalidated the handle before graphics shutdown");
        }
        require(!IsWindow(handle), "Native window survived platform destruction");

        {
            saige::Win32Platform platform(L"SAIGE Escape test", 320, 240);
            handle = reinterpret_cast<HWND>(platform.native_window().window);
            ShowWindow(handle, SW_HIDE);
            require(platform.poll_events(), "New platform inherited an earlier quit request");
            post(handle, WM_KEYDOWN, VK_ESCAPE);
            require(!platform.poll_events() && !platform.poll_events(), "Escape did not latch quit");
            require(IsWindow(handle), "Escape destroyed the window before graphics shutdown");
        }
        require(!IsWindow(handle), "Escape left a window alive after platform destruction");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
