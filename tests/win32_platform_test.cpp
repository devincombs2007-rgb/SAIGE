#include "saige/win32_platform.hpp"
#include "desktop_game.hpp"

#include <windows.h>

#include <chrono>
#include <cmath>
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

void test_course_input(saige::Win32Platform& platform, HWND window) {
    using namespace std::chrono_literals;
    constexpr auto step = 16'666'667ns;
    saige::World world;
    saige::sample::DesktopGame game(world, platform, true);
    const auto player_pose = [&] {
        const auto snapshot = world.snapshot();
        require(snapshot.size() == 4, "Course must expose the player and three obstacles");
        return snapshot.front().position;
    };
    const auto advance = [&] {
        require(platform.poll_events(), "Course window exited during input replay");
        game.fixed_update(world, step);
    };

    advance();
    const auto spawn = player_pose();
    const auto initial_camera = game.camera_view().value();
    const auto at_spawn = [&] {
        const auto pose = player_pose();
        return std::abs(pose.x - spawn.x) < 0.001f && std::abs(pose.y - spawn.y) < 0.001f &&
               std::abs(pose.z - spawn.z) < 0.001f; // The capsule may rest up to one contact skin above spawn.
    };
    post(window, WM_KEYDOWN, 'W');
    post(window, WM_KEYDOWN, VK_SHIFT);
    advance();
    for (int tick = 0; tick < 90; ++tick) game.fixed_update(world, step);
    const auto wall = player_pose();
    require(wall.x > 2.1f && wall.x < 2.201f && std::abs(wall.z - spawn.z) < 0.001f,
            "W and Shift must move the capsule to the wall without penetrating");

    post(window, WM_KEYUP, 'W');
    post(window, WM_KEYUP, VK_SHIFT);
    post(window, WM_KEYDOWN, VK_SPACE);
    advance();
    require(player_pose().z > wall.z + 0.02f, "Space must lift the capsule off the course floor");

    post(window, WM_KEYUP, VK_SPACE);
    post(window, WM_KEYDOWN, 'R');
    advance();
    require(at_spawn(), "R must restore the course spawn");
    post(window, WM_KEYUP, 'R');
    post(window, WM_KEYDOWN, 'W');
    post(window, WM_KILLFOCUS);
    advance();
    require(at_spawn(), "A pending key must not move the course player after focus loss");

    require(PostMessageW(window, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(20, 20)) != FALSE &&
                PostMessageW(window, WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(180, 60)) != FALSE &&
                PostMessageW(window, WM_RBUTTONUP, 0, MAKELPARAM(180, 60)) != FALSE,
            "Could not queue a course camera drag");
    advance();
    require(game.camera_view().value().position.y > initial_camera.position.y + 1.0f,
            "The Win32 right drag must reach the course camera");
    post(window, WM_KEYDOWN, 'W');
    advance();
    for (int tick = 0; tick < 15; ++tick) game.fixed_update(world, step);
    require(player_pose().x > 0.2f && player_pose().y < -0.1f,
            "Win32 walking follows the turned view");
    post(window, WM_KEYUP, 'W');
    post(window, WM_KEYDOWN, 'R');
    advance();
    require(at_spawn() && std::abs(game.camera_view().value().position.y - initial_camera.position.y) < 0.001f,
            "Course reset must also restore the camera");
    post(window, WM_KEYUP, 'R');
    require(platform.poll_events(), "Course window exited during reset");
}

void test_vehicle_input(saige::Win32Platform& platform, HWND window) {
    using namespace std::chrono_literals;
    saige::World world;
    saige::sample::DesktopGame game(world, platform, true, {}, {}, true);
    const auto advance = [&] {
        require(platform.poll_events(), "Vehicle course stopped during input replay");
        game.fixed_update(world, 16'666'667ns);
    };
    post(window, WM_KEYDOWN, 'D');
    for (int i = 0; i < 49; ++i) advance();
    post(window, WM_KEYUP, 'D');
    post(window, WM_KEYDOWN, 'E');
    advance();
    require(game.driving(), "Win32 E must transfer control to the nearby vehicle");
    post(window, WM_KEYUP, 'E');
    post(window, WM_KEYDOWN, 'W');
    for (int i = 0; i < 40; ++i) advance();
    require(world.snapshot().back().position.x > 1, "Win32 throttle must move the vehicle");
    post(window, WM_KEYUP, 'W');
    post(window, WM_KEYDOWN, VK_SPACE);
    for (int i = 0; i < 36; ++i) advance();
    post(window, WM_KEYDOWN, 'E');
    advance();
    require(!game.driving() && world.snapshot().front().position.z < 0.001f,
            "Win32 exit must restore the character on the ground");
    post(window, WM_KEYUP, 'E');
    post(window, WM_KEYUP, VK_SPACE);
    require(platform.poll_events(), "Vehicle course stopped after input replay");
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
            require(PostMessageW(handle, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(15, 20)) != FALSE &&
                        PostMessageW(handle, WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(24, 12)) != FALSE &&
                        PostMessageW(handle, WM_RBUTTONUP, 0, MAKELPARAM(24, 12)) != FALSE,
                    "Could not queue a right-button drag");
            require(platform.poll_events(), "Window quit during mouse input");
            const auto look = platform.consume_look_delta();
            const auto consumed = platform.consume_look_delta();
            require(look.x_pixels == 9 && look.y_pixels == -8 &&
                        consumed.x_pixels == 0 && consumed.y_pixels == 0,
                    "Mouse delta was lost, signed incorrectly, or consumed twice");
            ShowWindow(handle, SW_HIDE);

            post(handle, WM_KEYDOWN, 'W');
            require(platform.poll_events() && platform.key_down(saige::Key::w),
                    "A queued key press did not reach the input state");
            post(handle, WM_KEYUP, 'W');
            require(platform.poll_events() && !platform.key_down(saige::Key::w),
                    "A queued key release left movement active");

            post(handle, WM_KEYDOWN, VK_SPACE);
            post(handle, WM_KEYDOWN, VK_SHIFT);
            post(handle, WM_KEYDOWN, 'R');
            require(platform.poll_events() && platform.key_down(saige::Key::space) &&
                        platform.key_down(saige::Key::shift) && platform.key_down(saige::Key::r),
                    "Course controls did not reach the input state");
            post(handle, WM_KEYUP, VK_SPACE);
            require(platform.poll_events() && !platform.key_down(saige::Key::space),
                    "The jump key stayed active after release");

            post(handle, WM_KEYDOWN, VK_LEFT);
            require(platform.poll_events() && platform.key_down(saige::Key::left),
                    "A queued arrow key did not reach the input state");
            post(handle, WM_KILLFOCUS);
            require(platform.poll_events() && !platform.key_down(saige::Key::left) &&
                        !platform.key_down(saige::Key::shift) && !platform.key_down(saige::Key::r),
                    "A held movement key survived focus loss");

            test_course_input(platform, handle);
            test_vehicle_input(platform, handle);

            post(handle, WM_CLOSE);
            require(!platform.poll_events() && !platform.poll_events(), "Close did not latch quit");
            require(IsWindow(handle) && platform.native_window().window == native.window,
                    "Close invalidated the handle before graphics shutdown");
        }
        require(!IsWindow(handle), "Native window survived platform destruction");

        {
            saige::Win32Platform platform(L"SAIGE Escape test", 320, 240,
                                          saige::DesktopPresentation::renderer_paced);
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
