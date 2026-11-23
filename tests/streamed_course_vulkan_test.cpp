#include "streamed_course_renderer.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <algorithm>
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
        require(course.resident_count() == 1, "Expected just the reset chunk initially");
        const auto before = frame(0);
        require(!before.empty() && sink.stats().model_instances_drawn == 3, "The initial chunk must draw three imported instances");

        course.fixed_update(world, 16'666'667ns);
        require(course.resident_count() == 2 && sink.active_model_count() == 1, "Both chunks must share the staged GPU model");
        const auto after = frame(1);
        require(sink.stats().model_instances_drawn == 9 && sink.stats().unresolved_instances == 0,
                "The second frame must draw both chunks using resolved assets");
        require(after.size() == before.size() && after != before, "The newly resident chunk must change framebuffer pixels");
        require(frame(1) == after, "The composed scene must render consistently");
        sink.device().wait_idle();
        require(sink.device().validation_error_count() == 0, "Unexpected Vulkan validation errors");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
