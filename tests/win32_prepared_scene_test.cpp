#include "saige/prepared_scene.hpp"
#include "saige/win32_platform.hpp"

#include <windows.h>

#include <exception>
#include <iostream>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void post(HWND window, UINT message, WPARAM key = 0, LPARAM position = 0) {
    require(PostMessageW(window, message, key, position) != FALSE, "Posting a scene window message failed");
}

// Drive real Win32 input and surface events with one deterministic tick per pump.
class Desktop final : public saige::IDesktopPlatform {
public:
    saige::Win32Platform platform{L"SAIGE prepared scene test", 320, 240,
                                  saige::DesktopPresentation::renderer_paced};
    auto now() -> std::chrono::steady_clock::time_point override { return time_; }
    bool poll_events() override {
        time_ += 16'666'667ns;
        return platform.poll_events();
    }
    saige::NativeWindowHandle native_window() const noexcept override { return platform.native_window(); }
    saige::SurfaceState surface() const noexcept override { return platform.surface(); }
    bool key_down(saige::Key key) const noexcept override { return platform.key_down(key); }
    saige::LookDelta consume_look_delta() noexcept override { return platform.consume_look_delta(); }

private:
    std::chrono::steady_clock::time_point time_{};
};

class SceneProbe final : public saige::IFrameSink {
public:
    SceneProbe(saige::AssetId mesh, const Desktop& desktop) : mesh_(mesh), desktop_(desktop) {}

    saige::CameraView camera;
    saige::SurfaceState surface;
    int frames{};
    void submit(const saige::FrameView& frame) override {
        require(frame.camera && frame.instances.size() == 3, "Scene frame is missing its camera or instances");
        for (const auto& instance : frame.instances) {
            require(instance.mesh == mesh_ && instance.world_matrix, "Frame lost a mesh ID or transform");
        }
        const auto& child = *frame.instances[1].world_matrix;
        require(child[12] == 2.5F && child[13] == 0.3125F, "Frame lost its inherited transform");
        camera = *frame.camera;
        surface = desktop_.surface();
        ++frames;
    }

private:
    saige::AssetId mesh_;
    const Desktop& desktop_;
};

void test_scene(const std::filesystem::path& path) {
    saige::PreparedScene scene(path);
    require(scene.assets().size() == 1, "Scene did not share its imported mesh");
    HWND window{};
    {
        Desktop desktop;
        window = reinterpret_cast<HWND>(desktop.native_window().window);
        require(IsWindow(window), "Prepared scene test has no window");
        SceneProbe probe(scene.assets().front().id, desktop);
        require(scene.run(desktop, probe, 1).ticks == 1, "First Win32 scene tick failed");
        const auto initial_camera = probe.camera;
        const auto initial_surface = probe.surface;

        require(SetWindowPos(window, nullptr, 0, 0, 590, 410,
                             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
                "Resizing the scene window failed");
        require(scene.run(desktop, probe, 1).frames == 1 &&
                    probe.surface.revision > initial_surface.revision &&
                    probe.surface.width_px != initial_surface.width_px,
                "Resized extent did not reach frame submission");

        post(window, WM_KEYDOWN, 'W');
        require(scene.run(desktop, probe, 1).frames == 1 && probe.camera.position.x > initial_camera.position.x,
                "Native keyboard input did not move the camera");
        const auto moved = probe.camera.position.x;
        post(window, WM_KEYUP, 'W');
        post(window, WM_RBUTTONDOWN, MK_RBUTTON, MAKELPARAM(15, 20));
        post(window, WM_MOUSEMOVE, MK_RBUTTON, MAKELPARAM(24, 12));
        post(window, WM_RBUTTONUP, 0, MAKELPARAM(24, 12));
        require(scene.run(desktop, probe, 1).frames == 1 && probe.camera.position.x == moved &&
                    probe.camera.rotation.z != initial_camera.rotation.z,
                "Native key release or right-button look did not reach the camera");

        post(window, WM_KEYDOWN, 'W');
        post(window, WM_KILLFOCUS);
        require(scene.run(desktop, probe, 1).frames == 1 && probe.camera.position.x == moved,
                "Focus loss left the camera moving");

        const auto frames = probe.frames;
        post(window, WM_CLOSE);
        const auto stopped = scene.run(desktop, probe, 1);
        require(stopped.frames == 0 && stopped.ticks == 0 && probe.frames == frames,
                "Runtime updated or submitted a frame after close");
        require(IsWindow(window), "Close destroyed the surface before the frame sink could shut down");
    }
    require(!IsWindow(window), "Window survived desktop teardown");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Win32 scene test needs the fixture path");
        test_scene(std::filesystem::path(argv[1]));
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
