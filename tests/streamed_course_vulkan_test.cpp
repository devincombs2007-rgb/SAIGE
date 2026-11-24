#include "streamed_course_renderer.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>

namespace {

class TestDesktop final : public saige::IDesktopPlatform {
public:
    std::chrono::steady_clock::time_point now() override { return {}; }
    bool poll_events() override { return true; }
    saige::NativeWindowHandle native_window() const noexcept override { return {}; }
    saige::SurfaceState surface() const noexcept override { return {}; }
    bool key_down(saige::Key) const noexcept override { return false; }
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        using namespace std::chrono_literals;
        saige::rhi::DeviceDesc desc;
        desc.enable_validation = desc.require_validation = true;
        saige::render::RendererFrameSink sink(saige::rhi::create_vulkan_device(desc), 256, 192);
        saige::sample::StreamedCourseRenderer output(sink);
        saige::World world;
        TestDesktop desktop;
        saige::sample::StreamedCourse course(world, desktop, std::filesystem::path(argv[1]), &output);

        // Keep view, lighting and player out of the comparison. Only imports from the new chunk may change pixels.
        const saige::FreeCamera camera{.position = {-6, 0, 2}, .pitch_radians = -0.14f};
        const auto frame = [&](std::uint64_t tick) {
            auto instances = world.snapshot();
            std::erase_if(instances, [](const auto& instance) { return instance.mesh.is_builtin(); });
            sink.submit({tick, 0.0, instances, camera.view()});
            return sink.read_back_rgba();
        };
        require(course.resident_count() == 1 && output.uploads() == 1, "Expected just the reset chunk initially");
        const auto before = frame(0);
        require(!before.empty() && sink.stats().model_instances_drawn == 3, "The initial chunk must draw three imported instances");

        course.fixed_update(world, 16'666'667ns);
        require(course.resident_count() == 2 && sink.active_model_count() == 1 && output.uploads() == 1, "Both chunks must share the staged GPU model");
        const auto after = frame(1);
        require(sink.stats().model_instances_drawn == 9 && sink.stats().unresolved_instances == 0,
                "The second frame must draw both chunks using resolved assets");
        require(after.size() == before.size() && after != before, "The newly resident chunk must change framebuffer pixels");
        require(frame(1) == after, "The composed scene must render consistently");
        const auto sources = saige::load_chunk_manifest(argv[1]);
        const saige::PreparedScene fixture(sources.front().project);
        const auto& asset = fixture.assets().front();
        const auto source = (sources.front().project.parent_path() / fixture.project().assets.front().relative_path).lexically_normal();
        const std::array requested{
            saige::sample::CourseAsset{asset.id, &asset.model, source},
            saige::sample::CourseAsset{{asset.id.high, asset.id.low + 1}, &asset.model, source},
            saige::sample::CourseAsset{{asset.id.high, asset.id.low + 2}, &asset.model, source},
        };
        require(!output.stage(requested) && output.uploads() == 2 && sink.active_model_count() == 1,
                "First staging step uploads one missing asset and leaves the active scene unchanged");
        require(output.stage(requested) && output.uploads() == 3 && sink.active_model_count() == 1,
                "Second staging step finishes the candidate without publishing it");
        output.publish();
        require(sink.active_model_count() == 3, "Commit installs the prepared candidate");
        require(output.stage(std::span(requested).first(1)) && output.uploads() == 3,
                "Retiring assets must not reupload the retained model");
        output.publish();
        require(sink.active_model_count() == 1, "Commit retires the removed models");

        sink.device().wait_idle();
        require(sink.device().validation_error_count() == 0, "Unexpected Vulkan validation errors");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
