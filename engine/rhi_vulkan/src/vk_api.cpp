#include "vk_api.hpp"

#if defined(_WIN32)
#    include <windows.h>
#else
#    include <dlfcn.h>
#endif

namespace saige::rhi::vk {

namespace {

void* open_library()
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA("vulkan-1.dll"));
#elif defined(__APPLE__)
    void* lib = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
    return lib != nullptr ? lib : dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return lib != nullptr ? lib : dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* library, const char* name)
{
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

void close_library(void* library)
{
#if defined(_WIN32)
    FreeLibrary(reinterpret_cast<HMODULE>(library));
#else
    dlclose(library);
#endif
}

template <typename Fn>
Fn cast_fn(PFN_vkVoidFunction fn)
{
    return reinterpret_cast<Fn>(fn);
}

} // namespace

bool Api::load_library(std::string* error)
{
    library = open_library();
    if (library == nullptr) {
        if (error != nullptr) {
            *error = "Vulkan loader library not found (install a Vulkan-capable GPU driver)";
        }
        return false;
    }
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(find_symbol(library, "vkGetInstanceProcAddr"));
    if (vkGetInstanceProcAddr == nullptr) {
        if (error != nullptr) {
            *error = "vkGetInstanceProcAddr missing from the Vulkan loader";
        }
        unload();
        return false;
    }
#define SAIGE_VK_LOAD_GLOBAL(name) name = cast_fn<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
    SAIGE_VK_GLOBAL_FUNCTIONS(SAIGE_VK_LOAD_GLOBAL)
#undef SAIGE_VK_LOAD_GLOBAL
    if (vkCreateInstance == nullptr || vkEnumerateInstanceExtensionProperties == nullptr ||
        vkEnumerateInstanceLayerProperties == nullptr) {
        if (error != nullptr) {
            *error = "Vulkan loader is missing core global functions";
        }
        unload();
        return false;
    }
    return true;
}

bool Api::load_instance(VkInstance instance, std::string* error)
{
#define SAIGE_VK_LOAD_INSTANCE(name)                                                                                   \
    name = cast_fn<PFN_##name>(vkGetInstanceProcAddr(instance, #name));                                                \
    if (name == nullptr) {                                                                                             \
        if (error != nullptr) {                                                                                        \
            *error = "missing Vulkan instance function " #name;                                                        \
        }                                                                                                              \
        return false;                                                                                                  \
    }
    SAIGE_VK_INSTANCE_FUNCTIONS(SAIGE_VK_LOAD_INSTANCE)
#undef SAIGE_VK_LOAD_INSTANCE
#define SAIGE_VK_LOAD_OPTIONAL(name) name = cast_fn<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
    SAIGE_VK_OPTIONAL_INSTANCE_FUNCTIONS(SAIGE_VK_LOAD_OPTIONAL)
#undef SAIGE_VK_LOAD_OPTIONAL
    return true;
}

bool Api::load_device(VkDevice device, bool swapchain, std::string* error)
{
#define SAIGE_VK_LOAD_DEVICE(name)                                                                                     \
    name = cast_fn<PFN_##name>(vkGetDeviceProcAddr(device, #name));                                                    \
    if (name == nullptr) {                                                                                             \
        if (error != nullptr) {                                                                                        \
            *error = "missing Vulkan device function " #name;                                                          \
        }                                                                                                              \
        return false;                                                                                                  \
    }
    SAIGE_VK_DEVICE_FUNCTIONS(SAIGE_VK_LOAD_DEVICE)
    if (swapchain) {
        SAIGE_VK_SWAPCHAIN_FUNCTIONS(SAIGE_VK_LOAD_DEVICE)
    }
#undef SAIGE_VK_LOAD_DEVICE
    return true;
}

void Api::unload()
{
    if (library != nullptr) {
        close_library(library);
    }
    *this = Api{};
}

} // namespace saige::rhi::vk
