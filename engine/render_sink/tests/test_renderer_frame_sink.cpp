#include "saige/gfx_test/saige_test.hpp"

#include "saige/render_sink/renderer_frame_sink.hpp"
#include "saige/rhi_null/null_device.hpp"

#include <chrono>
#include <stdexcept>
#include <vector>

using namespace saige;
using namespace saige::render;
using namespace std::chrono_literals;

namespace {

/// IDesktopPlatform double: a fake Win32 window whose surface state tests change directly.
class FakeDesktop final : public saige::IDesktopPlatform {
public:
    saige::SurfaceState state{1280, 720, 1.0f, false, 1};
    saige::WindowSystem system = saige::WindowSystem::win32;
    std::chrono::steady_clock::time_point time{};
    int polls_left = 1000;

    std::chrono::steady_clock::time_point now() override { return time; }
    bool poll_events() override
    {
        time += 16ms;
        return polls_left-- > 0;
    }
    saige::NativeWindowHandle native_window() const noexcept override
    {
        return {system, reinterpret_cast<void*>(0x1), 0x2};
    }
    saige::SurfaceState surface() const noexcept override { return state; }
    bool key_down(saige::Key) const noexcept override { return false; }

    void resize(uint32_t w, uint32_t h, bool minimized = false)
    {
        state = {minimized ? 0u : w, minimized ? 0u : h, state.dpi_scale, minimized, state.revision + 1};
    }
};

struct WindowedFixture {
    FakeDesktop platform;
    rhi::NullDevice* device = nullptr;  // owned by the sink
    std::unique_ptr<RendererFrameSink> sink;

    WindowedFixture()
    {
        auto d = std::make_unique<rhi::NullDevice>();
        device = d.get();
        sink = std::make_unique<RendererFrameSink>(std::move(d), platform);
    }

    void submit(std::vector<saige::RenderInstance> instances = {{{0, 1}, {1, 2, 0}}}, uint64_t tick = 1)
    {
        sink->submit(saige::FrameView{tick, 0.0, instances});
    }
};

std::size_t count(const std::vector<rhi::RecordedCommand>& cmds, rhi::CommandType type)
{
    std::size_t n = 0;
    for (const auto& c : cmds) {
        n += c.type == type ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE("FrameSink: Core instances map to renderer instances with stable colors")
{
    const std::vector<saige::RenderInstance> core{{{0, 1}, {1.5f, -2.0f, 0.25f}}, {{7, 3}, {0, 0, 0}}};
    const std::vector<SceneInstance> scene = to_scene_instances(core);
    REQUIRE(scene.size() == 2);
    CHECK(scene[0].position == Float3{1.5f, -2.0f, 0.25f});
    CHECK(scene[0].color == Float3{0.85f, 0.30f, 0.10f});  // entity 0: the marker's orange
    CHECK(scene[1].color == to_scene_instances(std::vector<saige::RenderInstance>{{{7, 9}, {}}})[0].color);
    CHECK(to_scene_instances({}).empty());
}

TEST_CASE("FrameSink: windowed submit renders and presents one frame per FrameView")
{
    WindowedFixture f;
    f.submit();
    f.submit({});  // empty world is valid: ground only
    CHECK(f.sink->stats().frames_rendered == 2);
    CHECK(f.sink->stats().last_instance_count == 0);
    CHECK(f.device->present_count() == 2);
    CHECK(count(f.device->last_frame_commands(), rhi::CommandType::DrawIndexed) == 0);
    CHECK(count(f.device->last_frame_commands(), rhi::CommandType::Draw) == 1);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("FrameSink: a new surface revision resizes the swapchain")
{
    WindowedFixture f;
    f.submit();
    f.platform.resize(1920, 1080);
    f.submit();
    CHECK(f.sink->stats().swapchain_recreations == 1);
    CHECK(f.sink->width() == 1920);
    const auto& cmds = f.device->last_frame_commands();
    bool sized = false;
    for (const auto& c : cmds) {
        sized |= c.type == rhi::CommandType::BeginRendering && c.rendering.area.width == 1920 &&
                 c.rendering.area.height == 1080;
    }
    CHECK(sized);
    f.submit();  // same revision: no further recreation
    CHECK(f.sink->stats().swapchain_recreations == 1);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("FrameSink: minimized windows skip presentation; restore resumes rendering")
{
    WindowedFixture f;
    f.submit();
    f.platform.resize(0, 0, true);
    f.submit();
    f.submit();
    CHECK(f.sink->stats().frames_skipped == 2);
    CHECK(f.device->present_count() == 1);

    f.platform.resize(1280, 720);
    f.submit();
    CHECK(f.sink->stats().frames_rendered == 2);
    CHECK(f.device->present_count() == 2);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("FrameSink: an out-of-date swapchain is recreated on the next submit")
{
    WindowedFixture f;
    f.submit();
    // The sink does not expose its swapchain handle; the null device's first swapchain uses slot 0, generation 1.
    f.device->force_out_of_date(rhi::SwapchainHandle::from_parts(0, 1));
    f.submit();
    CHECK(f.sink->stats().frames_skipped == 1);
    f.submit();
    CHECK(f.sink->stats().swapchain_recreations == 1);
    CHECK(f.sink->stats().frames_rendered == 2);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("FrameSink: headless platforms are rejected for windowed rendering")
{
    FakeDesktop headless;
    headless.system = saige::WindowSystem::headless;
    bool threw = false;
    try {
        RendererFrameSink sink(std::make_unique<rhi::NullDevice>(), headless);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("FrameSink: drives the real Core runtime loop (saige::run) end to end")
{
    struct MovingMarker final : saige::IGame {
        saige::Entity marker;
        explicit MovingMarker(saige::World& w) : marker(w.create()) {}
        void fixed_update(saige::World& world, std::chrono::nanoseconds step) override
        {
            x += std::chrono::duration<float>(step).count();
            world.set_position(marker, {x, 0, 0});
        }
        float x = 0;
    };
    WindowedFixture f;
    saige::World world;
    MovingMarker game(world);
    const saige::RunStats stats = saige::run(world, f.platform, game, *f.sink, 30);
    CHECK(stats.frames == 30);
    CHECK(f.sink->stats().frames_submitted == 30);
    CHECK(f.sink->stats().frames_rendered == 30);
    CHECK(f.sink->stats().last_instance_count == 1);
    CHECK(f.device->validation_error_count() == 0);
}

TEST_CASE("FrameSink: offscreen mode renders and reads back")
{
    auto d = std::make_unique<rhi::NullDevice>();
    rhi::NullDevice* device = d.get();
    RendererFrameSink sink(std::move(d), 64, 36);
    CHECK(sink.read_back_rgba().empty());  // nothing rendered yet
    const std::vector<saige::RenderInstance> instances{{{0, 1}, {0, 0, 0}}};
    sink.submit(saige::FrameView{0, 0.0, instances});
    CHECK(sink.read_back_rgba().size() == 64u * 36u * 4u);
    CHECK(count(device->last_frame_commands(), rhi::CommandType::CopyTextureToBuffer) == 1);
    CHECK(device->validation_error_count() == 0);
}
