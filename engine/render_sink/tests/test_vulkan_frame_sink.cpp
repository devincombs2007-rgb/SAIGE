// End-to-end on Vulkan: Core's runtime (saige::run) -> RendererFrameSink -> Vulkan offscreen -> pixels.
// Exit code 77 when no Vulkan 1.3 device is available (reported by CTest as skipped).
// Set SAIGE_CAPTURE_DIR to save the first and last frames as PPM.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/render_sink/renderer_frame_sink.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace saige;
using namespace saige::render;
using namespace std::chrono_literals;

namespace {

struct ScriptedPlatform final : saige::IPlatform {
    std::chrono::steady_clock::time_point time{};
    std::chrono::steady_clock::time_point now() override { return time; }
    bool poll_events() override
    {
        time += 16'666'667ns;  // exactly one fixed step per frame
        return true;
    }
};

struct MovingMarker final : saige::IGame {
    saige::Entity marker;
    float speed;
    explicit MovingMarker(saige::World& w, float meters_per_second) : marker(w.create()), speed(meters_per_second) {}
    void fixed_update(saige::World& world, std::chrono::nanoseconds step) override
    {
        y += speed * std::chrono::duration<float>(step).count();
        world.set_position(marker, {0, y, 0});  // +Y is left in the SAIGE world
    }
    float y = 0;
};

struct Centroid {
    uint32_t count = 0;
    double x = 0;
};

Centroid orange_centroid(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h)
{
    Centroid c;
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = &rgba[(size_t{y} * w + x) * 4];
            if (p[0] > 80 && p[0] > p[1] + 40 && p[0] > p[2] + 60) {
                ++c.count;
                c.x += x;
            }
        }
    }
    if (c.count > 0) {
        c.x /= c.count;
    }
    return c;
}

void save(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h, const char* name)
{
    const char* dir = std::getenv("SAIGE_CAPTURE_DIR");
    if (dir == nullptr) {
        return;
    }
    std::ofstream out(std::string(dir) + "/" + name + ".ppm", std::ios::binary);
    out << "P6\n" << w << " " << h << "\n255\n";
    for (size_t i = 0; i < size_t{w} * h; ++i) {
        out.write(reinterpret_cast<const char*>(&rgba[i * 4]), 3);
    }
}

} // namespace

TEST_CASE("Vulkan sink: the runtime moves the marker and the rendered cube moves on screen")
{
    rhi::DeviceDesc dd;
    dd.enable_validation = true;
    auto device = rhi::create_vulkan_device(dd);
    rhi::Device* raw = device.get();

    FrameSinkSettings settings;
    settings.renderer.camera_mode = CameraMode::Fixed;  // fixed camera so motion shows up in screen space
    RendererFrameSink sink(std::move(device), 320, 180, settings);
    saige::World world;
    MovingMarker game(world, 3.0f);
    ScriptedPlatform platform;

    (void)saige::run(world, platform, game, sink, 1);
    const std::vector<uint8_t> first = sink.read_back_rgba();
    (void)saige::run(world, platform, game, sink, 30);  // +0.5 s => +1.5 m to the left
    const std::vector<uint8_t> last = sink.read_back_rgba();
    save(first, 320, 180, "runtime_first");
    save(last, 320, 180, "runtime_last");

    const Centroid a = orange_centroid(first, 320, 180);
    const Centroid b = orange_centroid(last, 320, 180);
    std::printf("  cube pixels %u -> %u, centroid x %.1f -> %.1f\n", a.count, b.count, a.x, b.x);
    CHECK(a.count > 300);
    CHECK(b.count > 300);
    CHECK(b.x < a.x - 15.0);  // moved left on screen
    CHECK(sink.stats().frames_rendered == 31);
    CHECK(raw->validation_error_count() == 0);
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
