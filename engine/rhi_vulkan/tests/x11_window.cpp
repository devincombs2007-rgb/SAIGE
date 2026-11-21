#include "x11_window.hpp"

#include <X11/Xlib.h>

namespace saige::test {

X11Window::~X11Window()
{
    if (display_ != nullptr) {
        auto* d = static_cast<Display*>(display_);
        if (window_ != 0) {
            XDestroyWindow(d, static_cast<Window>(window_));
        }
        XCloseDisplay(d);
    }
}

bool X11Window::open(uint32_t width, uint32_t height)
{
    Display* d = XOpenDisplay(nullptr);
    if (d == nullptr) {
        return false;
    }
    display_ = d;
    const Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, width, height, 0, 0, 0);
    window_ = static_cast<uintptr_t>(w);
    XMapWindow(d, w);
    XSync(d, False);
    return true;
}

void X11Window::resize(uint32_t width, uint32_t height)
{
    auto* d = static_cast<Display*>(display_);
    XResizeWindow(d, static_cast<Window>(window_), width, height);
    XSync(d, False);
}

} // namespace saige::test
