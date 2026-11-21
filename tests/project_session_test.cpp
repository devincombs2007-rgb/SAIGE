#include "saige/project_session.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {
using namespace std::chrono_literals;

void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

struct Platform final : saige::IDesktopPlatform {
    std::chrono::steady_clock::time_point time{};
    saige::LookDelta look{};
    std::chrono::steady_clock::time_point now() override { return time; }
    bool poll_events() override { time += 16'666'667ns; return true; }
    saige::NativeWindowHandle native_window() const noexcept override { return {}; }
    saige::SurfaceState surface() const noexcept override { return {}; }
    bool key_down(saige::Key key) const noexcept override { return key == saige::Key::w; }
    saige::LookDelta consume_look_delta() noexcept override { return std::exchange(look, {}); }
};

struct Sink final : saige::IFrameSink {
    int frames{};
    float initial_camera_x{};
    void submit(const saige::FrameView& frame) override {
        require(frame.instances.size() == 3 && frame.instances.front().world_matrix,
                "Project instance did not cross the runtime frame boundary");
        require(frame.camera && frame.camera->position.x > initial_camera_x,
                "Camera movement did not cross the runtime frame boundary");
        ++frames;
    }
};

void run(const std::filesystem::path& fixture) {
    Platform platform;
    saige::ProjectSession session(fixture, &platform);
    const auto before = session.camera_view()->position;
    require(session.world().snapshot().size() == 3, "Loaded scene did not create three entities");
    Sink sink;
    sink.initial_camera_x = before.x;
    const auto stats = saige::run(session.world(), platform, session, sink, 3);
    require(stats.ticks == 3 && sink.frames == 3, "Scripted scene did not run three frames");
    const auto moved = session.camera_view()->position;
    bool failed = false;
    try { session.reload(fixture.parent_path() / "missing-scene.json"); }
    catch (const std::runtime_error&) { failed = true; }
    require(failed && session.camera_view()->position == moved &&
                session.world().snapshot().size() == 3,
            "Failed reload damaged the active scene");
    session.reload(fixture);
    require(session.camera_view()->position == before, "Successful reload did not restore the saved camera");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Project fixture path required");
        run(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
