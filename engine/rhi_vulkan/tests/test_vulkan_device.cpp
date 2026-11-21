// Vulkan backend tests. Run on any Vulkan 1.3 device (CI uses Mesa lavapipe, a CPU implementation).
// Exit code 77 means "no suitable device here" and is reported by CTest as skipped, never as passed.
// Every test runs with validation enabled when the layer is installed and fails on any validation error.
//
// Set SAIGE_CAPTURE_DIR to write rendered frames as PPM images for inspection.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/scene_renderer.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#if defined(SAIGE_TEST_XLIB)
#    include "x11_window.hpp"
#endif

using namespace saige;
using namespace saige::render;

namespace {

std::unique_ptr<rhi::Device> make_device()
{
    rhi::DeviceDesc desc;
    desc.enable_validation = true;
    desc.application_name = "saige_rhi_vulkan_tests";
    return rhi::create_vulkan_device(desc);
}

struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;

    const uint8_t* at(uint32_t x, uint32_t y) const { return &rgba[(size_t{y} * width + x) * 4]; }
};

void write_ppm(const Image& img, const std::string& name)
{
    const char* dir = std::getenv("SAIGE_CAPTURE_DIR");
    if (dir == nullptr) {
        return;
    }
    std::ofstream out(std::string(dir) + "/" + name + ".ppm", std::ios::binary);
    out << "P6\n" << img.width << " " << img.height << "\n255\n";
    for (size_t i = 0; i < size_t{img.width} * img.height; ++i) {
        out.write(reinterpret_cast<const char*>(&img.rgba[i * 4]), 3);
    }
}

/// Offscreen color target + readback buffer.
struct Offscreen {
    rhi::Device& device;
    rhi::TextureHandle color;
    rhi::BufferHandle readback;
    uint32_t width, height;

    Offscreen(rhi::Device& d, uint32_t w, uint32_t h, rhi::Format format = rhi::Format::RGBA8_SRGB)
        : device(d), width(w), height(h)
    {
        rhi::TextureDesc td;
        td.format = format;
        td.width = w;
        td.height = h;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        td.debug_name = "TestTarget";
        color = device.create_texture(td);
        rhi::BufferDesc bd;
        bd.size = uint64_t{w} * h * 4;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        bd.debug_name = "TestReadback";
        readback = device.create_buffer(bd);
    }

    ~Offscreen()
    {
        device.destroy(readback);
        device.destroy(color);
    }

    Image read() const
    {
        const auto bytes = device.map(readback);
        Image img;
        img.width = width;
        img.height = height;
        img.rgba.assign(reinterpret_cast<const uint8_t*>(bytes.data()),
                        reinterpret_cast<const uint8_t*>(bytes.data()) + size_t{width} * height * 4);
        return img;
    }
};

bool is_cube_pixel(const uint8_t* p)
{
    // The default cube albedo is orange: red clearly dominates.
    return p[0] > 80 && p[0] > p[1] + 40 && p[0] > p[2] + 60;
}

bool is_sky_pixel(const uint8_t* p)
{
    return p[2] > p[0] + 30 && p[2] > 120;
}

struct Coverage {
    uint32_t count = 0;
    double cx = 0.0;
    double cy = 0.0;
};

Coverage cube_coverage(const Image& img)
{
    Coverage c;
    for (uint32_t y = 0; y < img.height; ++y) {
        for (uint32_t x = 0; x < img.width; ++x) {
            if (is_cube_pixel(img.at(x, y))) {
                ++c.count;
                c.cx += x;
                c.cy += y;
            }
        }
    }
    if (c.count > 0) {
        c.cx /= c.count;
        c.cy /= c.count;
    }
    return c;
}

Image render_scene(rhi::Device& device, SceneRenderer& renderer, Offscreen& target, const std::vector<SceneInstance>& inst)
{
    SceneFrame frame;
    frame.instances = inst;
    RenderTarget rt;
    rt.texture = target.color;
    rt.width = target.width;
    rt.height = target.height;
    rt.readback_buffer = target.readback;
    rhi::CommandList& cmd = device.begin_frame();
    (void)renderer.render(cmd, frame, rt);
    (void)device.end_frame();
    device.wait_idle();
    return target.read();
}

RendererSettings fixed_camera()
{
    RendererSettings s;
    s.camera_mode = CameraMode::Fixed;
    s.fixed_eye = {-6.0f, -4.5f, 4.0f};
    s.fixed_target = {0.0f, 0.0f, 0.5f};
    s.spin_radians_per_second = 0.0f;  // deterministic pose
    return s;
}

} // namespace

TEST_CASE("Vulkan: device creation reports a Vulkan 1.3 device")
{
    const auto device = make_device();
    const rhi::DeviceInfo& info = device->info();
    std::printf("  device: %s (Vulkan %u.%u, validation %s, software %s, presentation %s)\n", info.device_name.c_str(),
                info.api_version_major, info.api_version_minor, info.validation_enabled ? "on" : "off",
                info.software_device ? "yes" : "no", info.presentation_supported ? "yes" : "no");
    CHECK(info.backend == rhi::Backend::Vulkan);
    CHECK((info.api_version_major > 1 || info.api_version_minor >= 3));
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan: require_validation creates a validated device or fails loudly")
{
    rhi::DeviceDesc desc;
    desc.require_validation = true;
    try {
        const auto device = rhi::create_vulkan_device(desc);
        CHECK(device->info().validation_enabled);  // never silently unvalidated
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()).find("validation required") != std::string::npos);
    }
}

TEST_CASE("Vulkan: offscreen clear reads back the exact color")
{
    const auto device = make_device();
    {
        Offscreen target(*device, 64, 32, rhi::Format::RGBA8_UNORM);
        rhi::CommandList& cmd = device->begin_frame();
        const rhi::TextureBarrier to_attachment{
            target.color,
            {},
            {rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::ColorAttachmentWrite, rhi::TextureLayout::ColorAttachment},
            {}};
        cmd.barrier(std::span(&to_attachment, 1));
        rhi::RenderingDesc rd;
        rhi::ColorAttachment ca;
        ca.texture = target.color;
        ca.clear = {0.25f, 0.5f, 0.75f, 1.0f};
        rd.colors.push_back(ca);
        rd.area = {0, 0, 64, 32};
        cmd.begin_rendering(rd);
        cmd.end_rendering();
        const rhi::TextureBarrier to_transfer{
            target.color,
            to_attachment.after,
            {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc},
            {}};
        cmd.barrier(std::span(&to_transfer, 1));
        cmd.copy_texture_to_buffer(target.color, target.readback);
        (void)device->end_frame();
        device->wait_idle();

        const Image img = target.read();
        bool all_match = true;
        for (uint32_t y = 0; y < img.height; ++y) {
            for (uint32_t x = 0; x < img.width; ++x) {
                const uint8_t* p = img.at(x, y);
                all_match &= std::abs(p[0] - 64) <= 1 && std::abs(p[1] - 128) <= 1 && std::abs(p[2] - 191) <= 1 &&
                             p[3] == 255;
            }
        }
        CHECK(all_match);
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan: scene renderer draws a visible lit cube over sky and ground")
{
    const auto device = make_device();
    {
        Offscreen target(*device, 320, 180);
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, fixed_camera());
        const Image img = render_scene(*device, renderer, target, {SceneInstance{{0, 0, 0}}});
        write_ppm(img, "scene_cube");

        const Coverage c = cube_coverage(img);
        std::printf("  cube pixels: %u, centroid (%.1f, %.1f)\n", c.count, c.cx, c.cy);
        CHECK(c.count > 500);  // a 1 m cube at ~8 m fills a few percent of 320x180
        CHECK(std::fabs(c.cx - 160.0) < 25.0);  // camera looks at the cube
        CHECK(std::fabs(c.cy - 90.0) < 25.0);
        CHECK(is_sky_pixel(img.at(160, 2)));  // top center: sky
        const uint8_t* ground = img.at(160, 175);
        CHECK(!is_sky_pixel(ground));  // bottom center: ground, not sky
        CHECK(!is_cube_pixel(ground));
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan: world axes map to the screen as the coordinate standard requires")
{
    const auto device = make_device();
    {
        Offscreen target(*device, 320, 180);
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, fixed_camera());
        const Coverage origin = cube_coverage(render_scene(*device, renderer, target, {SceneInstance{{0, 0, 0}}}));
        const Image left_img = render_scene(*device, renderer, target, {SceneInstance{{0, 2, 0}}});
        write_ppm(left_img, "scene_cube_left");
        const Coverage left = cube_coverage(left_img);           // +Y is left
        const Coverage far = cube_coverage(render_scene(*device, renderer, target, {SceneInstance{{4, 0, 0}}}));  // +X forward
        const Coverage up = cube_coverage(render_scene(*device, renderer, target, {SceneInstance{{0, 0, 1}}}));   // +Z up

        CHECK(left.count > 0 && far.count > 0 && up.count > 0);
        CHECK(left.cx < origin.cx - 20.0);  // moving +Y moves the cube left on screen
        CHECK(far.count < origin.count);    // moving +X (away from a camera behind at -X) makes it smaller
        CHECK(far.cy < origin.cy);          // ... and higher on screen (further along the ground)
        CHECK(up.cy < origin.cy - 10.0);    // moving +Z moves it up on screen
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan: multiple frames in flight, resizes and resource churn stay validation-clean")
{
    const auto device = make_device();
    {
        SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB);
        std::vector<SceneInstance> instances{{{0, 0, 0}}, {{2, 1, 0}, {0.2f, 0.6f, 0.9f}, 0.5f}};
        for (uint32_t frame = 0; frame < 12; ++frame) {
            const uint32_t w = frame < 6 ? 128u : 96u;  // the depth buffer is recreated mid-run
            rhi::TextureDesc td;
            td.format = rhi::Format::RGBA8_SRGB;
            td.width = w;
            td.height = 64;
            td.usage = rhi::TextureUsage::ColorAttachment;
            const rhi::TextureHandle color = device->create_texture(td);
            SceneFrame sf;
            sf.tick = frame;
            sf.instances = instances;
            RenderTarget rt;
            rt.texture = color;
            rt.width = w;
            rt.height = 64;
            rhi::CommandList& cmd = device->begin_frame();
            (void)renderer.render(cmd, sf, rt);
            (void)device->end_frame();
            device->destroy(color);  // still in flight: destruction must be deferred
            instances[0].position.x += 0.25f;
        }
        CHECK(device->frame_number() == 12);
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

#if defined(SAIGE_TEST_XLIB)
TEST_CASE("Vulkan: Xlib swapchain presents, resizes, minimizes and recreates (needs DISPLAY, e.g. Xvfb)")
{
    test::X11Window window;
    if (!window.open(320, 240)) {
        std::printf("  skipped: no X display (set DISPLAY, e.g. run under xvfb-run)\n");
        return;
    }
    const auto device = make_device();
    if (!device->info().presentation_supported) {
        std::printf("  skipped: device has no presentation support\n");
        return;
    }
    {
        rhi::SwapchainDesc sd;
        sd.surface = {rhi::NativeSurface::System::Xlib, window.display(), window.window()};
        sd.width = 320;
        sd.height = 240;
        const rhi::SwapchainHandle sc = device->create_swapchain(sd);
        const rhi::Format format = device->swapchain_format(sc);
        CHECK(format != rhi::Format::Unknown);
        SceneRenderer renderer(*device, format);
        const std::vector<SceneInstance> instances{{{0, 0, 0}}};

        auto present_frames = [&](int count, uint32_t w, uint32_t h) {
            int presented = 0;
            for (int i = 0; i < count; ++i) {
                rhi::CommandList& cmd = device->begin_frame();
                const rhi::AcquireResult img = device->acquire_next_image(sc);
                if (img.status == rhi::AcquireStatus::Success) {
                    SceneFrame sf;
                    sf.tick = static_cast<uint64_t>(i);
                    sf.instances = instances;
                    RenderTarget rt;
                    rt.texture = img.texture;
                    rt.width = w;
                    rt.height = h;
                    rt.initial_state = {rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::None,
                                        rhi::TextureLayout::Undefined};
                    rt.final_state = rhi::ResourceState{rhi::PipelineStage::None, rhi::Access::None,
                                                        rhi::TextureLayout::Present};
                    (void)renderer.render(cmd, sf, rt);
                }
                const rhi::FrameResult r = device->end_frame();
                presented += r.presented ? 1 : 0;
                if (r.swapchain_out_of_date || img.status == rhi::AcquireStatus::OutOfDate) {
                    device->resize_swapchain(sc, w, h);
                }
            }
            return presented;
        };

        CHECK(present_frames(5, 320, 240) == 5);

        window.resize(400, 300);
        device->resize_swapchain(sc, 400, 300);
        CHECK(present_frames(5, 400, 300) >= 4);

        device->resize_swapchain(sc, 0, 0);  // minimized: nothing to present
        CHECK(present_frames(2, 0, 0) == 0);

        device->resize_swapchain(sc, 400, 300);  // restored
        CHECK(present_frames(3, 400, 300) >= 2);

        device->wait_idle();
        device->destroy(sc);  // surface released before the window is destroyed (D-18 order)
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}  // window destroyed after the surface (D-18 order)
#endif

int main(int argc, char** argv)
{
    std::string reason;
    if (!rhi::vulkan_device_available(&reason)) {
        std::printf("SKIPPED: no usable Vulkan 1.3 device: %s\n", reason.c_str());
        return 77;
    }
    return saige::test::run_all(argc, argv);
}
