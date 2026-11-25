// SAIGE RHI Vulkan 1.3 backend (docs/rendering/rhi-design.md).
//
// Requirements: Vulkan 1.3 with dynamicRendering, synchronization2 and timelineSemaphore. Presentation uses
// VK_KHR_surface + VK_KHR_win32_surface (Windows) or VK_KHR_xlib_surface (Linux/X11, when built with Xlib).
// No Vulkan types appear in this header; the loader is opened at runtime.
#pragma once

#include "saige/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace saige::rhi {

/// Creates a Vulkan device. Throws std::runtime_error with a readable reason if no suitable driver/device
/// exists (no loader, Vulkan < 1.3, missing features, or presentation required but unavailable).
[[nodiscard]] std::unique_ptr<Device> create_vulkan_device(const DeviceDesc& desc);

/// Probes for a usable Vulkan 1.3 device without keeping it. Returns false and a reason when unavailable.
[[nodiscard]] bool vulkan_device_available(std::string* reason = nullptr);

#if defined(SAIGE_RHI_VULKAN_TESTING)
/// Test-only fault injection, compiled only into builds that build the tests (SAIGE_RHI_VULKAN_TESTING). Not for production use: it exists so tests can exercise the rollback of the backend's own
/// bookkeeping, whose failure (an allocation of CPU memory) cannot otherwise be provoked deterministically.
namespace testing {
/// Makes the n-th (1-based) device-memory registration made after this call throw std::bad_alloc, as if the CPU-side ledger
/// could not grow. 0 cancels. Process-wide; the hook is consumed by the registration it fails.
void fail_memory_bookkeeping_after(uint32_t n) noexcept;
/// Validation errors reported by every Vulkan device in this process since start, including those a device reports while
/// it is being destroyed (leaked child objects), which no per-device counter can be read after.
[[nodiscard]] uint32_t process_validation_error_count() noexcept;
/// Makes the n-th (1-based) create_bind_group made after this call throw std::bad_alloc immediately after the backend has acquired
/// its descriptor set, as a failed host allocation there would. 0 cancels. Process-wide; consumed by the call it fails.
void fail_bind_group_after_acquisition(uint32_t n) noexcept;
/// The same for the step after the descriptor writes, immediately before the group's handle is published (the handle pool's own
/// allocation). 0 cancels.
void fail_bind_group_before_publication(uint32_t n) noexcept;
/// Makes the n-th (1-based) creation of a new descriptor pool made after this call throw std::bad_alloc at the point where the backend
/// makes room to track it (before the native pool exists), as a failed vector growth there would. 0 cancels. Process-wide; consumed by
/// the creation it fails.
void fail_descriptor_pool_tracking_after(uint32_t n) noexcept;
/// Descriptor sets a Vulkan device has acquired from the driver and not yet freed (0 for any other device).
[[nodiscard]] std::size_t descriptor_sets_in_use(const Device& device);
/// Makes every Vulkan device in this process report that it cannot blend into `format` (as a device lacking
/// VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT for it would), so the rejection path can be tested on a device that supports it.
/// Format::Unknown cancels.
void deny_blending_into(Format format) noexcept;
/// Makes every Vulkan device in this process report that it cannot sample `format` with linear filtering (as a device lacking
/// VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT for it would). Format::Unknown cancels.
void deny_filtered_sampling_of(Format format) noexcept;
} // namespace testing
#endif

} // namespace saige::rhi
