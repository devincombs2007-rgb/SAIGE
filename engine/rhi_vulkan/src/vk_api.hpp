// SAIGE Vulkan backend — runtime loader for the Vulkan API (no link-time dependency on vulkan-1 / libvulkan).
//
// The loader library is opened at runtime and every entry point is fetched through vkGetInstanceProcAddr /
// vkGetDeviceProcAddr, like volk. Builds therefore need only the Vulkan headers; the GPU driver provides the
// loader at run time.
#pragma once

#ifndef VK_NO_PROTOTYPES
#    define VK_NO_PROTOTYPES
#endif

#if defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    define VK_USE_PLATFORM_WIN32_KHR
#elif defined(SAIGE_VULKAN_XLIB)
#    define VK_USE_PLATFORM_XLIB_KHR
#endif

#include <vulkan/vulkan.h>

#if defined(VK_USE_PLATFORM_XLIB_KHR)
// Xlib defines short macros that collide with ordinary identifiers (rhi::Access::None, CompareOp::Always, ...).
#    undef None
#    undef Success
#    undef Always
#    undef Bool
#    undef Status
#endif

#include <string>

namespace saige::rhi::vk {

#define SAIGE_VK_GLOBAL_FUNCTIONS(X)                                                                                   \
    X(vkCreateInstance)                                                                                                \
    X(vkEnumerateInstanceExtensionProperties)                                                                          \
    X(vkEnumerateInstanceLayerProperties)                                                                              \
    X(vkEnumerateInstanceVersion)

#define SAIGE_VK_INSTANCE_FUNCTIONS(X)                                                                                 \
    X(vkDestroyInstance)                                                                                               \
    X(vkEnumeratePhysicalDevices)                                                                                      \
    X(vkGetPhysicalDeviceProperties)                                                                                   \
    X(vkGetPhysicalDeviceFeatures2)                                                                                    \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    X(vkGetPhysicalDeviceMemoryProperties2)                                                                            \
    X(vkGetPhysicalDeviceFormatProperties)                                                                             \
    X(vkEnumerateDeviceExtensionProperties)                                                                            \
    X(vkCreateDevice)                                                                                                  \
    X(vkGetDeviceProcAddr)

#define SAIGE_VK_OPTIONAL_INSTANCE_FUNCTIONS(X)                                                                        \
    X(vkCreateDebugUtilsMessengerEXT)                                                                                  \
    X(vkDestroyDebugUtilsMessengerEXT)                                                                                 \
    X(vkSetDebugUtilsObjectNameEXT)                                                                                    \
    X(vkCmdBeginDebugUtilsLabelEXT)                                                                                    \
    X(vkCmdEndDebugUtilsLabelEXT)                                                                                      \
    X(vkDestroySurfaceKHR)                                                                                             \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)                                                                       \
    SAIGE_VK_PLATFORM_SURFACE_FUNCTIONS(X)

#if defined(VK_USE_PLATFORM_WIN32_KHR)
#    define SAIGE_VK_PLATFORM_SURFACE_FUNCTIONS(X) X(vkCreateWin32SurfaceKHR)
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
#    define SAIGE_VK_PLATFORM_SURFACE_FUNCTIONS(X) X(vkCreateXlibSurfaceKHR)
#else
#    define SAIGE_VK_PLATFORM_SURFACE_FUNCTIONS(X)
#endif

#define SAIGE_VK_DEVICE_FUNCTIONS(X)                                                                                   \
    X(vkDestroyDevice)                                                                                                 \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkDeviceWaitIdle)                                                                                                \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkGetBufferMemoryRequirements)                                                                                   \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkBindBufferMemory)                                                                                              \
    X(vkMapMemory)                                                                                                     \
    X(vkUnmapMemory)                                                                                                   \
    X(vkCreateImage)                                                                                                   \
    X(vkDestroyImage)                                                                                                  \
    X(vkGetImageMemoryRequirements)                                                                                    \
    X(vkBindImageMemory)                                                                                               \
    X(vkCreateImageView)                                                                                               \
    X(vkDestroyImageView)                                                                                              \
    X(vkCreateShaderModule)                                                                                            \
    X(vkDestroyShaderModule)                                                                                           \
    X(vkCreatePipelineLayout)                                                                                          \
    X(vkDestroyPipelineLayout)                                                                                         \
    X(vkCreateGraphicsPipelines)                                                                                       \
    X(vkDestroyPipeline)                                                                                               \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkResetCommandPool)                                                                                              \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkCreateSemaphore)                                                                                               \
    X(vkDestroySemaphore)                                                                                              \
    X(vkWaitSemaphores)                                                                                                \
    X(vkGetSemaphoreCounterValue)                                                                                      \
    X(vkQueueSubmit2)                                                                                                  \
    X(vkCmdPipelineBarrier2)                                                                                           \
    X(vkCmdBeginRendering)                                                                                             \
    X(vkCmdEndRendering)                                                                                               \
    X(vkCmdSetViewport)                                                                                                \
    X(vkCmdSetScissor)                                                                                                 \
    X(vkCmdBindPipeline)                                                                                               \
    X(vkCmdPushConstants)                                                                                              \
    X(vkCmdBindVertexBuffers)                                                                                          \
    X(vkCmdBindIndexBuffer)                                                                                            \
    X(vkCmdDraw)                                                                                                       \
    X(vkCmdDrawIndexed)                                                                                                \
    X(vkCmdCopyBuffer)                                                                                                 \
    X(vkCmdCopyBufferToImage)                                                                                          \
    X(vkCreateQueryPool)                                                                                               \
    X(vkDestroyQueryPool)                                                                                              \
    X(vkCmdResetQueryPool)                                                                                             \
    X(vkCmdWriteTimestamp2)                                                                                            \
    X(vkGetQueryPoolResults)                                                                                           \
    X(vkCreateSampler)                                                                                                 \
    X(vkCreateDescriptorSetLayout)                                                                                     \
    X(vkDestroyDescriptorSetLayout)                                                                                    \
    X(vkCreateDescriptorPool)                                                                                          \
    X(vkDestroyDescriptorPool)                                                                                         \
    X(vkAllocateDescriptorSets)                                                                                        \
    X(vkFreeDescriptorSets)                                                                                            \
    X(vkUpdateDescriptorSets)                                                                                          \
    X(vkCmdBindDescriptorSets)                                                                                         \
    X(vkDestroySampler)                                                                                                \
    X(vkCmdCopyImageToBuffer)

#define SAIGE_VK_SWAPCHAIN_FUNCTIONS(X)                                                                                \
    X(vkCreateSwapchainKHR)                                                                                            \
    X(vkDestroySwapchainKHR)                                                                                           \
    X(vkGetSwapchainImagesKHR)                                                                                         \
    X(vkAcquireNextImageKHR)                                                                                           \
    X(vkQueuePresentKHR)

/// Function table. Global and instance entry points are filled by load_*(); missing optional ones stay null.
struct Api {
    void* library = nullptr;
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;

#define SAIGE_VK_DECLARE(name) PFN_##name name = nullptr;
    SAIGE_VK_GLOBAL_FUNCTIONS(SAIGE_VK_DECLARE)
    SAIGE_VK_INSTANCE_FUNCTIONS(SAIGE_VK_DECLARE)
    SAIGE_VK_OPTIONAL_INSTANCE_FUNCTIONS(SAIGE_VK_DECLARE)
    SAIGE_VK_DEVICE_FUNCTIONS(SAIGE_VK_DECLARE)
    SAIGE_VK_SWAPCHAIN_FUNCTIONS(SAIGE_VK_DECLARE)
#undef SAIGE_VK_DECLARE

    /// Opens the Vulkan loader and resolves the global functions. Returns false with a reason on failure.
    bool load_library(std::string* error);
    /// Resolves instance-level functions (required ones must exist; returns false otherwise).
    bool load_instance(VkInstance instance, std::string* error);
    /// Resolves device-level functions. `swapchain` loads the VK_KHR_swapchain entry points too.
    bool load_device(VkDevice device, bool swapchain, std::string* error);
    void unload();
};

} // namespace saige::rhi::vk
