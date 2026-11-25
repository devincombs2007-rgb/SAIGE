# Selection of the Vulkan headers the backend builds against (engine/rhi_vulkan/CMakeLists.txt).
#
# saige_vulkan_headers_usable(<include_dir> <out_var>) sets <out_var> to TRUE when a translation unit that includes
# <vulkan/vulkan_core.h> from exactly <include_dir> compiles with the Vulkan 1.4 core API: VK_VERSION_1_4 defined, VK_HEADER_VERSION at
# least SAIGE_VULKAN_MIN_HEADER_VERSION, and VkPhysicalDeviceVulkan14Features (with its structure type) declared. Checking by
# compiling the tree that will actually be used, rather than reading a version macro from a file that another tree might shadow,
# is what makes a selected header set trustworthy: the build is given that same directory.
function(saige_vulkan_headers_usable include_dir out_var)
    string(MD5 _hash "${include_dir}")
    set(_probe_dir "${CMAKE_BINARY_DIR}/saige_vulkan_header_probe/${_hash}")
    file(MAKE_DIRECTORY "${_probe_dir}")
    set(_source "${_probe_dir}/probe.cpp")
    file(WRITE "${_source}"
"#include <vulkan/vulkan_core.h>
#if !defined(VK_VERSION_1_4)
#error \"the Vulkan headers do not define VK_VERSION_1_4\"
#endif
#if VK_HEADER_VERSION < ${SAIGE_VULKAN_MIN_HEADER_VERSION}
#error \"the Vulkan headers are older than the required release\"
#endif
int main()
{
    VkPhysicalDeviceVulkan14Features features{};
    features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
    return static_cast<int>(features.sType == VK_STRUCTURE_TYPE_MAX_ENUM);
}
")
    set(_result_var saige_vulkan_headers_probe_${_hash})
    unset(${_result_var} CACHE)  # never reuse an answer from an earlier configure or another tree
    try_compile(${_result_var} "${_probe_dir}/build"
        SOURCES "${_source}"
        CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${include_dir}"
        OUTPUT_VARIABLE _output)
    if(${_result_var})
        set(${out_var} TRUE PARENT_SCOPE)
    else()
        set(${out_var} FALSE PARENT_SCOPE)
        file(WRITE "${_probe_dir}/last_failure.txt" "${_output}")
    endif()
endfunction()
