#pragma once

#include "saige/desktop.hpp"

#include <memory>

namespace saige {

// Owns one top-level Win32 window. Construct, poll, and destroy on the UI thread.
class Win32Platform final : public IDesktopPlatform {
public:
    explicit Win32Platform(const wchar_t* title = L"SAIGE", int width = 960, int height = 540);
    ~Win32Platform() override;
    Win32Platform(const Win32Platform&) = delete;
    Win32Platform& operator=(const Win32Platform&) = delete;

    std::chrono::steady_clock::time_point now() override;
    bool poll_events() override;
    [[nodiscard]] NativeWindowHandle native_window() const noexcept override;
    [[nodiscard]] SurfaceState surface() const noexcept override;
    [[nodiscard]] bool key_down(Key key) const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saige
