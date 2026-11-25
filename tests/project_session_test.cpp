#include "saige/project_session.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <random>
#include <utility>

namespace {
using namespace std::chrono_literals;

void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

struct Platform final : saige::IDesktopPlatform {
    std::chrono::steady_clock::time_point time{};
    saige::LookDelta look{};
    int polls{};
    std::chrono::steady_clock::time_point now() override { return time; }
    bool poll_events() override {
        ++polls;
        if (polls > 3) return false;
        time += 16'666'667ns;
        if (polls == 2) look = {-24.F, 8.F};
        return true;
    }
    saige::NativeWindowHandle native_window() const noexcept override { return {}; }
    saige::SurfaceState surface() const noexcept override { return {}; }
    bool key_down(saige::Key key) const noexcept override { return key == saige::Key::w; }
    saige::LookDelta consume_look_delta() noexcept override { return std::exchange(look, {}); }
};

struct Sink final : saige::IFrameSink {
    int frames{};
    float initial_camera_x{};
    saige::AssetId mesh{};
    bool saw_turn{};
    void submit(const saige::FrameView& frame) override {
        require(frame.instances.size() == 3 && frame.instances.front().world_matrix,
                "Project instance did not cross the runtime frame boundary");
        require(frame.camera && frame.camera->position.x > initial_camera_x,
                "Camera movement did not cross the runtime frame boundary");
        const auto& child = frame.instances[1];
        require(frame.instances[0].mesh == mesh && child.mesh == mesh &&
                    frame.instances[2].mesh == mesh,
                "Shared asset identity was lost during extraction");
        require(child.world_matrix && (*child.world_matrix)[12] == 2.5F &&
                    (*child.world_matrix)[13] == 0.3125F,
                "Child position did not inherit its parent scale");
        if (frame.camera->rotation.z != 0.F) saw_turn = true;
        ++frames;
    }
};

struct Metrics final : saige::ICpuFrameObserver {
    std::uint64_t samples{};
    void record(const saige::CpuFrameMetrics& frame) override {
        require(frame.frame == ++samples && frame.ticks == 1 && frame.instance_count == 3,
                "CPU metrics diverged from recorded frame");
        require(frame.total == frame.input + frame.simulation + frame.extraction + frame.submission &&
                    frame.input >= 0ns && frame.simulation >= 0ns &&
                    frame.extraction >= 0ns && frame.submission >= 0ns,
                "CPU phases do not account for the measured frame");
    }
};

struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("saige-session-test-" + std::to_string(std::random_device{}()));
    TemporaryDirectory() { require(std::filesystem::create_directory(path), "Cannot create test directory"); }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

void run(const std::filesystem::path& fixture) {
    Platform platform;
    saige::ProjectSession session(fixture, &platform);
    const auto before = session.camera_view()->position;
    require(session.world().snapshot().size() == 3, "Loaded scene did not create three entities");
    Sink sink;
    sink.initial_camera_x = before.x;
    sink.mesh = session.source().assets.at(0).id;
    Metrics metrics;
    const auto stats = saige::run(session.world(), platform, session, sink, std::nullopt, {}, &metrics);
    require(stats.ticks == 3 && sink.frames == 3 && metrics.samples == 3 &&
                platform.polls == 4 && sink.saw_turn,
            "Scripted camera, metrics or shutdown diverged from the frame trace");
    const auto moved = session.camera_view()->position;
    bool failed = false;
    TemporaryDirectory staged;
    const auto copied = staged.path / fixture.filename();
    std::filesystem::copy_file(fixture, copied);
    try { session.reload(copied); }
    catch (const std::runtime_error&) { failed = true; }
    require(failed && session.camera_view()->position == moved &&
                session.world().snapshot().size() == 3,
            "Failed reload damaged the active scene");
    std::filesystem::copy_file(fixture.parent_path() / "checker-box.gltf", staged.path / "checker-box.gltf");
    auto saved = session.source();
    saved.camera.position.z += 2.F;
    saved.nodes.front().local.scale = {2.F, 2.F, 2.F};
    saige::save_project(saved, copied);
    session.reload(copied);
    const auto reopened = session.world().snapshot();
    require(session.camera_view()->position == saved.camera.position &&
                session.source().assets.front().id == sink.mesh && reopened[1].mesh == sink.mesh &&
                reopened[1].world_matrix && (*reopened[1].world_matrix)[12] == 4.F &&
                (*reopened[1].world_matrix)[13] == 0.5F,
            "Reopen did not restore the saved camera, shared asset identity, and inherited transform");
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
