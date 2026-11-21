#pragma once

#include "saige/runtime.hpp"

#include <cstdint>

namespace saige {

enum class WindowSystem { headless, win32 };

// Borrowed on the host thread. For win32, display is HINSTANCE and window is HWND.
// Release all renderer surfaces before the platform is destroyed.
struct NativeWindowHandle {
    WindowSystem system{WindowSystem::headless};
    void* display{};
    std::uintptr_t window{};
};

struct SurfaceState {
    std::uint32_t width_px{};
    std::uint32_t height_px{};
    float dpi_scale{1.0F};
    bool minimized{};
    std::uint64_t revision{};
};

enum class Key { up, down, left, right, w, a, s, d, escape };

// Query these values on the same thread that owns and pumps the platform.
// A changed revision indicates a new surface state. Minimized extents are zero.
// Key state describes the most recent message pump and clears on focus loss.
class IDesktopPlatform : public IPlatform {
public:
    [[nodiscard]] virtual NativeWindowHandle native_window() const noexcept = 0;
    [[nodiscard]] virtual SurfaceState surface() const noexcept = 0;
    [[nodiscard]] virtual bool key_down(Key key) const noexcept = 0;
};

} // namespace saige
