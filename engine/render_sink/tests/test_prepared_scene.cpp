// The native app's scene flow on Vulkan, minus the Win32 window: PreparedScene (Core) -> stage_prepared_scene ->
// activate_scene -> PreparedScene::run -> RendererFrameSink, driven by a fake desktop that holds W. Covers first load, camera
// movement through Core's input, a failed candidate (previous scene keeps drawing), replacement with frames in flight, and
// leak-free teardown. Exit code 77 when no Vulkan 1.3 device is available. Validation errors fail each test.

#include "saige/gfx_test/image.hpp"
#include "saige/gfx_test/png.hpp"
#include "saige/gfx_test/saige_test.hpp"

#include "saige/prepared_scene.hpp"
#include "saige/render_sink/prepared_scene.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace saige;
using namespace saige::render;
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kW = 320;
constexpr uint32_t kH = 180;

fs::path fixture() { return fs::path(SAIGE_TEXTURED_SCENE_DIR) / "saige.scene.json"; }

std::unique_ptr<rhi::Device> make_device(const char* name)
{
    rhi::DeviceDesc desc;
    desc.enable_validation = true;
    desc.enable_synchronization_validation = true;
    const char* require = std::getenv("SAIGE_REQUIRE_VALIDATION");
    desc.require_validation = require != nullptr && require[0] != '\0' && require[0] != '0';
    desc.application_name = name;
    return rhi::create_vulkan_device(desc);
}

FrameSinkSettings settings()
{
    FrameSinkSettings s;
    s.renderer.fog_density = 0.0f;
    s.renderer.spin_radians_per_second = 0.0f;
    return s;
}

/// Core's IDesktopPlatform without a window: a fixed clock, W held, nothing else.
class Desktop final : public saige::IDesktopPlatform {
public:
    bool forward = true;
    auto now() -> std::chrono::steady_clock::time_point override { return time_; }
    bool poll_events() override
    {
        time_ += std::chrono::nanoseconds(16'666'667);
        return true;
    }
    saige::NativeWindowHandle native_window() const noexcept override { return {}; }
    saige::SurfaceState surface() const noexcept override { return {}; }
    bool key_down(saige::Key key) const noexcept override { return forward && key == saige::Key::w; }
    saige::LookDelta consume_look_delta() noexcept override { return {}; }

private:
    std::chrono::steady_clock::time_point time_{};
};

saige::test::Image read(RendererFrameSink& sink)
{
    saige::test::Image img;
    img.width = sink.width();
    img.height = sink.height();
    img.rgba = sink.read_back_rgba();
    return img;
}

void capture(const saige::test::Image& img, const char* name)
{
    if (const char* dir = std::getenv("SAIGE_CAPTURE_DIR")) {
        (void)saige::test::write_ppm(std::string(dir) + "/" + name + ".ppm", img);
    }
}

/// Number of pixels whose color differs between two frames by more than `threshold` in any channel.
uint32_t differing(const saige::test::Image& a, const saige::test::Image& b, int threshold = 12)
{
    uint32_t count = 0;
    for (uint32_t y = 0; y < a.height; ++y) {
        for (uint32_t x = 0; x < a.width; ++x) {
            int delta = 0;
            for (int c = 0; c < 3; ++c) delta = std::max(delta, std::abs(int{a.at(x, y)[c]} - int{b.at(x, y)[c]}));
            if (delta > threshold) ++count;
        }
    }
    return count;
}

struct TempProject {
    fs::path dir = fs::temp_directory_path() / ("saige-native-flow-" + std::to_string(std::random_device{}()));
    fs::path scene;
    TempProject()
    {
        fs::create_directory(dir);
        for (const fs::directory_entry& e : fs::directory_iterator(fs::path(SAIGE_TEXTURED_SCENE_DIR))) {
            if (e.is_regular_file()) fs::copy_file(e.path(), dir / e.path().filename());
        }
        scene = dir / "saige.scene.json";
    }
    ~TempProject()
    {
        std::error_code ignored;
        fs::remove_all(dir, ignored);
    }
};

} // namespace

TEST_CASE("Native flow: prepare, stage, activate and run renders the saved scene and moves with Core's input")
{
    Desktop desktop;
    PreparedScene prepared(fixture());
    auto sink = std::make_unique<RendererFrameSink>(make_device("native_flow"), kW, kH, settings());

    StagedScene staged = stage_prepared_scene(*sink, prepared);
    CHECK(staged.model_count() == prepared.assets().size());
    CHECK(staged.report().warnings.empty());
    sink->activate_scene(std::move(staged));

    const RunStats first = prepared.run(desktop, *sink, 2);
    CHECK(first.frames == 2);
    CHECK(sink->stats().frames_rendered == 2);
    CHECK(sink->stats().unresolved_instances == 0);  // every instance's stable ID resolved to a staged model
    CHECK(sink->stats().model_instances_drawn == 6);  // three saved placements, two frames
    const saige::test::Image early = read(*sink);
    capture(early, "native_first");

    desktop.forward = true;
    (void)prepared.run(desktop, *sink, 60);  // W held: the saved camera moves
    const saige::test::Image later = read(*sink);
    capture(later, "native_camera_moved");
    CHECK(differing(early, later) > 200);  // the view changed substantially
    CHECK(sink->diagnostics().validation_errors == 0);

    sink->device().wait_idle();
    CHECK(sink->diagnostics().validation_errors == 0);
}

TEST_CASE("Native flow: a candidate that fails to prepare leaves the active scene drawing")
{
    TempProject project;
    Desktop desktop;
    desktop.forward = false;
    PreparedScene active(project.scene);
    auto sink = std::make_unique<RendererFrameSink>(make_device("native_flow_failed"), kW, kH, settings());
    sink->activate_scene(stage_prepared_scene(*sink, active));
    (void)active.run(desktop, *sink, 1);
    const saige::test::Image before = read(*sink);
    const ResourceStats resources_before = sink->diagnostics().resources;

    // Break the asset on disk: the CPU preparation of the candidate throws before any GPU work.
    {
        std::ofstream broken(project.dir / active.project().assets.front().relative_path, std::ios::binary);
        broken << "{invalid";
    }
    bool threw = false;
    try {
        PreparedScene candidate(project.scene);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);

    // The renderer was never touched: same resources, same pixels.
    CHECK(sink->diagnostics().resources.live == resources_before.live);
    (void)active.run(desktop, *sink, 1);
    CHECK(differing(before, read(*sink), 0) == 0);
    CHECK(sink->diagnostics().validation_errors == 0);
}

TEST_CASE("Native flow: replacing a scene while frames are in flight retires the old resources without leaks")
{
    Desktop desktop;
    desktop.forward = false;
    auto first = std::make_unique<PreparedScene>(fixture());
    auto sink = std::make_unique<RendererFrameSink>(make_device("native_flow_replace"), kW, kH, settings());
    sink->activate_scene(stage_prepared_scene(*sink, *first));
    (void)first->run(desktop, *sink, 3);
    const uint32_t live_with_one_scene = sink->diagnostics().resources.live;
    CHECK(live_with_one_scene > 0);

    PreparedScene second(fixture());
    StagedScene candidate = stage_prepared_scene(*sink, second);
    // Both scenes are resident while the candidate is staged and the old one is still active.
    CHECK(sink->diagnostics().resources.live > live_with_one_scene);
    sink->activate_scene(std::move(candidate));  // frames of `first` may still be running on the GPU
    CHECK(sink->diagnostics().resources.live == live_with_one_scene);  // the old scene's references are gone
    (void)second.run(desktop, *sink, 3);
    CHECK(sink->stats().unresolved_instances == 0);
    CHECK(sink->diagnostics().validation_errors == 0);

    // The old CPU bundle can go away at any time: nothing on the GPU side refers to it.
    first.reset();
    (void)second.run(desktop, *sink, 1);
    CHECK(sink->diagnostics().validation_errors == 0);
    // Tearing the sink down with the scene active reports no leak (checked by the sink's destructor).
}

int main(int argc, char** argv)
{
    std::string reason;
    if (!rhi::vulkan_device_available(&reason)) {
        std::printf("SKIPPED: no usable Vulkan 1.3 device: %s\n", reason.c_str());
        return 77;
    }
    return saige::test::run_all(argc, argv);
}
