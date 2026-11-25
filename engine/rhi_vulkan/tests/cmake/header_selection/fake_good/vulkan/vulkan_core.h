// Stand-in for a header set that declares the Vulkan 1.4 API the backend needs.
#pragma once
#define VK_HEADER_VERSION 363
#define VK_VERSION_1_3 1
#define VK_VERSION_1_4 1
typedef enum VkStructureType { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES = 55, VK_STRUCTURE_TYPE_MAX_ENUM = 0x7FFFFFFF } VkStructureType;
typedef struct VkPhysicalDeviceVulkan14Features { VkStructureType sType; } VkPhysicalDeviceVulkan14Features;
