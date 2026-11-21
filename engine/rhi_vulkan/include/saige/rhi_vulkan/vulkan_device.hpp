// SAIGE RHI Vulkan 1.3 backend (docs/rendering/rhi-design.md).
//
// Requirements: Vulkan 1.3 with dynamicRendering, synchronization2 and timelineSemaphore. Presentation uses
// VK_KHR_surface + VK_KHR_win32_surface (Windows) or VK_KHR_xlib_surface (Linux/X11, when built with Xlib).
// No Vulkan types appear in this header; the loader is opened at runtime.
#pragma once

#include "saige/rhi/device.hpp"

#include <memory>
#include <string>

namespace saige::rhi {

/// Creates a Vulkan device. Throws std::runtime_error with a readable reason if no suitable driver/device
/// exists (no loader, Vulkan < 1.3, missing features, or presentation required but unavailable).
[[nodiscard]] std::unique_ptr<Device> create_vulkan_device(const DeviceDesc& desc);

/// Probes for a usable Vulkan 1.3 device without keeping it. Returns false and a reason when unavailable.
[[nodiscard]] bool vulkan_device_available(std::string* reason = nullptr);

} // namespace saige::rhi
