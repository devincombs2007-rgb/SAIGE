#include "saige/prepared_scene.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>

namespace {
using namespace std::chrono_literals;
namespace fs = std::filesystem;

void require(bool result, const char* message) {
    if (!result) throw std::runtime_error(message);
}

struct StageDirectory {
    fs::path path = fs::temp_directory_path() / ("saige-prepared-test-" + std::to_string(std::random_device{}()));
    StageDirectory() { require(fs::create_directory(path), "Cannot create a staged project directory"); }
    ~StageDirectory() { std::error_code ignored; fs::remove_all(path, ignored); }
};

class Desktop final : public saige::IDesktopPlatform {
public:
    bool should_run = true;
    int input_reads{};

    auto now() -> std::chrono::steady_clock::time_point override { return time_; }
    bool poll_events() override {
        time_ += 16'666'667ns;
        return should_run;
    }
    saige::NativeWindowHandle native_window() const noexcept override { return {}; }
    saige::SurfaceState surface() const noexcept override { return {}; }
    bool key_down(saige::Key key) const noexcept override { return key == saige::Key::w; }
    saige::LookDelta consume_look_delta() noexcept override {
        ++input_reads;
        return {};
    }
private:
    std::chrono::steady_clock::time_point time_{};
};

class Observer final : public saige::ICpuFrameObserver {
public:
    int records{};
    void record(const saige::CpuFrameMetrics& frame) override {
        require(frame.instance_count == 3, "CPU observer lost scene instances");
        ++records;
    }
};

class FailingSink final : public saige::IFrameSink {
public:
    void submit(const saige::FrameView&) override { throw std::runtime_error("Sink failed"); }
};

class Sink final : public saige::IFrameSink {
public:
    explicit Sink(saige::AssetId mesh) : mesh_(mesh) {}
    int frames{};
    float camera_x{};
    void submit(const saige::FrameView& frame) override {
        require(frame.camera.has_value() && frame.instances.size() == 3, "Prepared scene did not submit a view");
        for (const auto& instance : frame.instances) {
            require(instance.mesh == mesh_ && instance.world_matrix, "Prepared scene lost a renderable identity or pose");
        }
        const auto& child = *frame.instances[1].world_matrix;
        require(child[12] == 2.5F && child[13] == 0.3125F, "Prepared scene did not resolve the parent transform");
        ++frames;
        camera_x = frame.camera->position.x;
    }
private:
    saige::AssetId mesh_;
};

template<class Load>
void require_failure(Load load, const char* message) {
    try {
        load();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

void test_scene(const fs::path& fixture) {
    Desktop desktop;
    auto active = std::make_unique<saige::PreparedScene>(fixture);
    require(active->assets().size() == 1, "Shared mesh was not imported once");
    const auto id = active->project().assets.front().id;
    require(active->find_asset(id) == &active->assets().front().model && !active->find_asset({}),
            "Scene asset lookup did not match the stable ID");
    require(active->asset_payload_bytes() > active->assets().front().model.images.front().bytes.size(),
            "Cached asset bytes did not include the geometry");
    require_failure([&] { (void)saige::PreparedScene(fixture, 0); }, "Scene ignored the payload budget");

    Sink sink(id);
    Observer observer;
    require(active->run(desktop, sink, 0).frames == 0 && desktop.input_reads == 0,
            "Zero-budget run consumed input");
    require(active->run(desktop, sink, 2, &observer).ticks == 2 && observer.records == 2, "Scene did not advance camera input");
    const float moved = sink.camera_x;
    const int input_reads = desktop.input_reads;

    StageDirectory staged;
    const auto candidate = staged.path / fixture.filename();
    const auto asset_path = staged.path / active->project().assets.front().relative_path;
    fs::copy_file(fixture, candidate);
    require_failure([&] { active = std::make_unique<saige::PreparedScene>(candidate); },
                    "A project with a missing asset was staged");
    {
        std::ofstream broken(asset_path, std::ios::binary);
        broken << "{invalid";
    }
    require_failure([&] { active = std::make_unique<saige::PreparedScene>(candidate); },
                    "A malformed glTF asset was accepted");
    require(desktop.input_reads == input_reads, "Failed staging consumed the active scene's input");
    require(active->run(desktop, sink, 1).frames == 1 && sink.camera_x > moved,
            "Failed staging interrupted the running scene");

    FailingSink failing_sink;
    require_failure([&] { active->run(desktop, failing_sink, 1, &observer); },
                    "Runtime swallowed the sink error");
    Desktop recovered_desktop;
    require(observer.records == 2 && active->run(recovered_desktop, sink, 1).frames == 1 &&
                recovered_desktop.input_reads == 2,
            "Prepared scene could not rebind input after a failed submission");

    fs::copy_file(fixture.parent_path() / asset_path.filename(), asset_path, fs::copy_options::overwrite_existing);
    auto replacement = std::make_unique<saige::PreparedScene>(candidate);
    require(replacement->find_asset(id) != nullptr, "Recovered project lost its stable asset identity");
    active.swap(replacement);
    desktop.should_run = false;
    require(active->run(desktop, sink, 2).frames == 0 && sink.frames == 4,
            "Prepared scene submitted a frame after quit");
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::invalid_argument("Prepared scene test needs the fixture path");
        test_scene(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
