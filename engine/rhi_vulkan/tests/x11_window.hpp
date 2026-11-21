// Test-only X11 window (isolated so Xlib macros never leak into test code). Not an engine platform layer:
// Core's 0.1 desktop host is Win32; this exists so the swapchain path can be exercised on Linux under Xvfb.
#pragma once

#include <cstdint>

namespace saige::test {

class X11Window {
public:
    X11Window() = default;
    ~X11Window();
    X11Window(const X11Window&) = delete;
    X11Window& operator=(const X11Window&) = delete;

    bool open(uint32_t width, uint32_t height);  ///< false if no X display is available
    void resize(uint32_t width, uint32_t height);
    [[nodiscard]] void* display() const noexcept { return display_; }
    [[nodiscard]] uintptr_t window() const noexcept { return window_; }

private:
    void* display_ = nullptr;
    uintptr_t window_ = 0;
};

} // namespace saige::test
