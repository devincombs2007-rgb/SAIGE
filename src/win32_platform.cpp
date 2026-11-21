#include "saige/win32_platform.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace saige {
namespace {

[[noreturn]] void fail_win32(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}

class WindowClass {
public:
    static constexpr wchar_t name[] = L"SAIGE.Core.Desktop";
    HINSTANCE instance = GetModuleHandleW(nullptr);

    explicit WindowClass(WNDPROC proc) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.hInstance = instance;
        wc.lpszClassName = name;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
        if (!RegisterClassW(&wc)) {
            fail_win32("RegisterClassW");
        }
    }

    ~WindowClass() { UnregisterClassW(name, instance); }
};

// HWNDs inherit their creating thread's DPI context. Restore the caller's
// context after setup so other windows on that thread keep their own policy.
struct WindowDpiContext {
    DPI_AWARENESS_CONTEXT previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WindowDpiContext() {
        if (!previous) {
            fail_win32("SetThreadDpiAwarenessContext");
        }
    }
    ~WindowDpiContext() { SetThreadDpiAwarenessContext(previous); }
};

std::optional<Key> key_for(WPARAM vk) {
    switch (vk) {
    case VK_UP:     return Key::up;
    case VK_DOWN:   return Key::down;
    case VK_LEFT:   return Key::left;
    case VK_RIGHT:  return Key::right;
    case 'W':       return Key::w;
    case 'A':       return Key::a;
    case 'S':       return Key::s;
    case 'D':       return Key::d;
    case VK_ESCAPE: return Key::escape;
    default:        return std::nullopt;
    }
}

} // namespace

struct Win32Platform::Impl {
    HWND window{};
    HINSTANCE instance{};
    SurfaceState surface;
    std::array<bool, static_cast<std::size_t>(Key::escape) + 1> keys{};
    bool closing{};
    std::chrono::steady_clock::time_point next_frame{};

    Impl(const wchar_t* title, int width, int height) {
        constexpr int size_limit = std::numeric_limits<int>::max() / 2;
        if (!title || width <= 0 || width > size_limit || height <= 0 || height > size_limit) {
            throw std::invalid_argument("Win32Platform requires a title and positive client dimensions");
        }
        const WindowDpiContext dpi_context;
        static const WindowClass window_class(window_proc);
        instance = window_class.instance;
        constexpr DWORD style = WS_OVERLAPPEDWINDOW;
        RECT rect{0, 0, width, height};
        if (!AdjustWindowRectExForDpi(&rect, style, FALSE, 0, GetDpiForSystem())) {
            fail_win32("AdjustWindowRectExForDpi");
        }
        window = CreateWindowExW(0, WindowClass::name, title, style, CW_USEDEFAULT, CW_USEDEFAULT,
                                 rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, this);
        if (!window) {
            fail_win32("CreateWindowExW");
        }
        ShowWindow(window, SW_SHOWNORMAL);
        update_surface(window, IsIconic(window) != FALSE, GetDpiForWindow(window));
    }

    ~Impl() {
        if (window) {
            DestroyWindow(window);
        }
    }

    void update_surface(HWND hwnd, bool minimized, UINT dpi) {
        RECT rect{};
        if (!GetClientRect(hwnd, &rect)) {
            return;
        }
        const auto width = minimized ? 0U : static_cast<std::uint32_t>(rect.right - rect.left);
        const auto height = minimized ? 0U : static_cast<std::uint32_t>(rect.bottom - rect.top);
        const float scale = dpi ? static_cast<float>(dpi) / 96.0F : surface.dpi_scale;
        if (surface.width_px != width || surface.height_px != height ||
            surface.minimized != minimized || surface.dpi_scale != scale) {
            surface = {width, height, scale, minimized, surface.revision + 1};
        }
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            self = static_cast<Impl*>(reinterpret_cast<const CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) {
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
        switch (msg) {
        case WM_CLOSE:
            // Consumers release GPU surfaces after run() returns, before our destructor.
            self->closing = true;
            return 0;
        case WM_NCDESTROY:
            self->closing = true;
            self->window = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            break;
        case WM_SIZE:
            self->update_surface(hwnd, wp == SIZE_MINIMIZED, GetDpiForWindow(hwnd));
            return 0;
        case WM_DPICHANGED: {
            const auto* rect = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, rect->left, rect->top, rect->right - rect->left,
                         rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            self->update_surface(hwnd, IsIconic(hwnd) != FALSE, LOWORD(wp));
            return 0;
        }
        case WM_KILLFOCUS:
            self->keys.fill(false);
            break;
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP: {
            if (const auto key = key_for(wp)) {
                const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
                self->keys[static_cast<std::size_t>(*key)] = down;
                if (*key == Key::escape && down) {
                    self->closing = true;
                }
            }
            break; // Preserve system key handling, including Alt+F4.
        }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
};

Win32Platform::Win32Platform(const wchar_t* title, int width, int height)
    : impl_(std::make_unique<Impl>(title, width, height)) {}
Win32Platform::~Win32Platform() = default;

std::chrono::steady_clock::time_point Win32Platform::now() {
    return std::chrono::steady_clock::now();
}

bool Win32Platform::poll_events() {
    if (impl_->closing) {
        return false;
    }
    std::this_thread::sleep_until(impl_->next_frame);
    impl_->next_frame = now() + std::chrono::nanoseconds(16'666'667);
    MSG message{};
    // Bound work per frame so an input flood cannot indefinitely starve simulation.
    for (unsigned i = 0; i < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
        if (message.message == WM_QUIT) {
            impl_->closing = true;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (impl_->closing) {
            break;
        }
    }
    return !impl_->closing;
}

NativeWindowHandle Win32Platform::native_window() const noexcept {
    return {WindowSystem::win32, impl_->instance, reinterpret_cast<std::uintptr_t>(impl_->window)};
}

SurfaceState Win32Platform::surface() const noexcept { return impl_->surface; }

bool Win32Platform::key_down(Key key) const noexcept {
    const auto index = static_cast<std::size_t>(key);
    return index < impl_->keys.size() && impl_->keys[index];
}

} // namespace saige
