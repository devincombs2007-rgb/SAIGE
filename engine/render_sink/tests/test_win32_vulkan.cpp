// Windows end-to-end: Core's Win32Platform window -> RendererFrameSink -> Vulkan swapchain (VK_KHR_win32_surface).
//
// Exercises the contract in docs/core-rendering-contract.md with a real HWND: presentation, resize, minimize/restore,
// held-input-free quit, and the required shutdown order (GPU work, swapchain and surface released while the HWND is
// still valid; the HWND destroyed afterwards). Runs on any Vulkan 1.3 device, including a software ICD such as Mesa
// lavapipe on hosted CI runners. Exit code 77 = no usable Vulkan device (reported by CTest as skipped, not passed).
//
// Environment:
//   SAIGE_REQUIRE_VALIDATION=1   fail instead of continuing when the validation layer is missing
//   SAIGE_CAPTURE_DIR=<dir>      write the offscreen frame and a best-effort window capture as PPM

#include "saige/gfx_test/saige_test.hpp"

#include "saige/render_sink/renderer_frame_sink.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"
#include "saige/win32_platform.hpp"

#ifndef NOMINMAX
#    define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace saige;

namespace {

bool env_flag(const char* name)
{
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != '\0' && v[0] != '0';
}

std::unique_ptr<rhi::Device> make_device()
{
    rhi::DeviceDesc desc;
    desc.enable_validation = true;
    desc.require_validation = env_flag("SAIGE_REQUIRE_VALIDATION");
    desc.require_presentation = true;
    desc.application_name = "saige_win32_vulkan_tests";
    return rhi::create_vulkan_device(desc);
}

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

/// What a window capture showed. `cube_pixels` counts orange pixels (the marker cube's albedo), which a blank (white
/// or black) window or stale contents cannot produce, so it distinguishes "the renderer's output is on screen" from
/// "some pixels exist".
struct WindowCapture {
    bool ok = false;
    int width = 0;
    int height = 0;
    size_t cube_pixels = 0;
    size_t sky_pixels = 0;
};

/// Reads the window's client area back with GDI. Software WSI presents through GDI, and the runner has an interactive
/// console session, so BitBlt from the window DC sees what was presented. Optionally saved as PPM.
WindowCapture capture_window(HWND hwnd, const char* name)
{
    WindowCapture result;
    const char* dir = std::getenv("SAIGE_CAPTURE_DIR");
    RECT rc{};
    if (!GetClientRect(hwnd, &rc)) {
        return result;
    }
    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        return result;
    }
    HDC window_dc = GetDC(hwnd);
    HDC mem_dc = CreateCompatibleDC(window_dc);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(window_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem_dc, bitmap);
    const BOOL ok = BitBlt(mem_dc, 0, 0, w, h, window_dc, 0, 0, SRCCOPY);
    result.width = w;
    result.height = h;
    if (ok != FALSE && bits != nullptr) {
        result.ok = true;
        const auto* px = static_cast<const uint8_t*>(bits);
        std::ofstream out;
        if (dir != nullptr) {
            out.open(std::string(dir) + "\\" + name + ".ppm", std::ios::binary);
            out << "P6\n" << w << " " << h << "\n255\n";
        }
        for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
            const int b = px[i * 4 + 0];
            const int g = px[i * 4 + 1];
            const int r = px[i * 4 + 2];
            if (out.is_open()) {
                const uint8_t rgb[3] = {static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)};
                out.write(reinterpret_cast<const char*>(rgb), 3);
            }
            result.cube_pixels += (r > 80 && r > g + 40 && r > b + 60) ? 1u : 0u;
            result.sky_pixels += (b > r + 30 && b > 120) ? 1u : 0u;
        }
    }
    std::printf("  window capture '%s': %dx%d, BitBlt %s, cube pixels %zu, sky-like pixels %zu\n", name, w, h,
                ok != FALSE ? "ok" : "failed", result.cube_pixels, result.sky_pixels);
    SelectObject(mem_dc, old);
    DeleteObject(bitmap);
    DeleteDC(mem_dc);
    ReleaseDC(hwnd, window_dc);
    return result;
}

void pump_frames(saige::World& world, saige::Win32Platform& platform, saige::IGame& game, saige::IFrameSink& sink,
                 std::uint64_t frames)
{
    (void)saige::run(world, platform, game, sink, frames);
}

} // namespace

TEST_CASE("Win32 Vulkan: device reports its identity (which driver does this run exercise?)")
{
    const auto device = make_device();
    const rhi::DeviceInfo& info = device->info();
    std::printf("  device: %s | Vulkan %u.%u | software=%s | validation=%s | presentation=%s\n",
                info.device_name.c_str(), info.api_version_major, info.api_version_minor,
                info.software_device ? "yes" : "no", info.validation_enabled ? "on" : "OFF",
                info.presentation_supported ? "yes" : "no");
    CHECK(info.presentation_supported);
    CHECK(info.backend == rhi::Backend::Vulkan);
    if (env_flag("SAIGE_REQUIRE_VALIDATION")) {
        CHECK(info.validation_enabled);
    }
}

TEST_CASE("Win32 Vulkan: real HWND presents, resizes, minimizes, restores and shuts down in contract order")
{
    HWND hwnd = nullptr;
    {
        saige::Win32Platform platform(L"SAIGE Win32 Vulkan test", 640, 360, saige::DesktopPresentation::renderer_paced);
        hwnd = reinterpret_cast<HWND>(platform.native_window().window);
        REQUIRE(IsWindow(hwnd) != FALSE);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);

        saige::World world;
        MovingMarker game(world);
        auto device = make_device();
        rhi::Device* raw = device.get();
        // Declared after the platform, destroyed before it (D-18).
        auto sink = std::make_unique<render::RendererFrameSink>(std::move(device), platform);
        const saige::SurfaceState initial = platform.surface();
        REQUIRE(initial.width_px > 0);
        std::printf("  initial client area: %ux%u (dpi scale %.2f)\n", initial.width_px, initial.height_px,
                    static_cast<double>(initial.dpi_scale));

        // 1. Steady presentation.
        pump_frames(world, platform, game, *sink, 20);
        CHECK(sink->stats().frames_rendered == 20);
        const WindowCapture initial_capture = capture_window(hwnd, "win32_window_initial");
        REQUIRE(initial_capture.ok);
        CHECK(initial_capture.cube_pixels > 300);  // the presented frame, not a blank window, is on screen
        CHECK(initial_capture.sky_pixels > 300);

        // 2. Resize: the new surface revision recreates the swapchain at the new extent.
        REQUIRE(SetWindowPos(hwnd, nullptr, 0, 0, 900, 560, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
        const saige::SurfaceState resized = platform.surface();  // WM_SIZE is delivered synchronously on this thread
        CHECK(resized.revision > initial.revision);
        const std::uint64_t recreations_before = sink->stats().swapchain_recreations;
        pump_frames(world, platform, game, *sink, 10);
        CHECK(sink->stats().swapchain_recreations > recreations_before);
        CHECK(sink->width() == resized.width_px);
        CHECK(sink->height() == resized.height_px);
        CHECK(sink->stats().frames_rendered == 30);
        const WindowCapture resized_capture = capture_window(hwnd, "win32_window_resized");
        CHECK(resized_capture.ok);
        CHECK(resized_capture.width == static_cast<int>(resized.width_px));
        CHECK(resized_capture.cube_pixels > 300);  // re-presented at the new size

        // 3. Minimize: zero extent means the sink must skip acquire/present (contract), and keep running.
        ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
        const std::uint64_t rendered_before_min = sink->stats().frames_rendered;
        pump_frames(world, platform, game, *sink, 6);
        CHECK(platform.surface().minimized);
        CHECK(sink->stats().frames_rendered == rendered_before_min);
        CHECK(sink->stats().frames_skipped >= 6);

        // 4. Restore: rendering resumes on a recreated swapchain.
        ShowWindow(hwnd, SW_RESTORE);
        pump_frames(world, platform, game, *sink, 12);
        CHECK(!platform.surface().minimized);
        CHECK(sink->stats().frames_rendered > rendered_before_min);
        const WindowCapture restored_capture = capture_window(hwnd, "win32_window_restored");
        CHECK(restored_capture.ok);
        CHECK(restored_capture.cube_pixels > 300);

        // 5. Close request latches quit; the HWND stays valid until the platform is destroyed.
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        const saige::RunStats closing = saige::run(world, platform, game, *sink, 50);
        CHECK(closing.frames < 50);  // stopped by the quit request, not the frame budget
        CHECK(IsWindow(hwnd) != FALSE);

        CHECK(raw->validation_error_count() == 0);
        std::printf("  frames rendered %llu, skipped %llu, swapchain recreations %llu\n",
                    static_cast<unsigned long long>(sink->stats().frames_rendered),
                    static_cast<unsigned long long>(sink->stats().frames_skipped),
                    static_cast<unsigned long long>(sink->stats().swapchain_recreations));

        // 6. Release GPU work, swapchain and surface while the HWND is still alive.
        sink.reset();
        CHECK(IsWindow(hwnd) != FALSE);
    }
    CHECK(IsWindow(hwnd) == FALSE);  // platform destruction destroyed the window after the surface was gone
}

TEST_CASE("Win32 Vulkan: a sink that fails to start leaves the platform and window intact")
{
    saige::Win32Platform platform(L"SAIGE Win32 Vulkan failure test", 320, 240);
    const HWND hwnd = reinterpret_cast<HWND>(platform.native_window().window);
    ShowWindow(hwnd, SW_HIDE);
    bool threw = false;
    try {
        render::RendererFrameSink sink(nullptr, platform);  // null device is rejected before any window access
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(IsWindow(hwnd) != FALSE);
}

int main(int argc, char** argv)
{
    std::string reason;
    if (!rhi::vulkan_device_available(&reason)) {
        std::printf("SKIPPED: no usable Vulkan 1.3 device on this machine: %s\n", reason.c_str());
        return 77;
    }
    return saige::test::run_all(argc, argv);
}
