# saige::rhi_vulkan: Vulkan RHI backend (docs/rendering/rhi-design.md). Vulkan 1.3 is the required runtime baseline;
# newer API versions are negotiated at run time (docs/rendering/vulkan-adapter-matrix.md).
#
# Only the Vulkan *headers* are needed at build time: the loader (vulkan-1.dll / libvulkan.so.1) is opened at
# runtime, so builds have no link dependency on the Vulkan SDK. Header sources, in order:
#   1. find_package(Vulkan) (Vulkan SDK or distro package such as libvulkan-dev), if its headers are at least
#      SAIGE_VULKAN_MIN_HEADER_VERSION; older system headers cannot describe Vulkan 1.4 and are skipped
#   2. SAIGE_VULKAN_FETCH_HEADERS=ON: Khronos Vulkan-Headers at the pinned SDK release tag via FetchContent (Apache-2.0)
# Header version is a *build* property only: it never raises the runtime requirement above Vulkan 1.3.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_rhi_vulkan LANGUAGES CXX)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_RHI_VULKAN_BUILD_TESTS "Build saige::rhi_vulkan tests" ${_saige_tests_default})
option(SAIGE_VULKAN_FETCH_HEADERS "Fetch Khronos Vulkan-Headers when no Vulkan SDK/package is found" OFF)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)
if(NOT TARGET saige::rhi)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../rhi ${CMAKE_CURRENT_BINARY_DIR}/rhi)
endif()

# First header release that defines the Vulkan 1.4 core API (VK_VERSION_1_4, VkPhysicalDeviceVulkan14Features).
set(SAIGE_VULKAN_MIN_HEADER_VERSION 303)
# The SDK-aligned release tag fetched when no sufficiently new headers are installed. Tag vulkan-sdk-1.4.363.0 is commit
# 6802bb4733b63ed5efd3adb308a6c885ef180ea1 of KhronosGroup/Vulkan-Headers.
set(SAIGE_VULKAN_HEADERS_TAG vulkan-sdk-1.4.363.0)

# VK_HEADER_VERSION of the installed headers, or 0 if there are none. Probed by path (not through find_package) because the
# fetched Vulkan-Headers defines its own Vulkan::Headers target, which cannot coexist with the one FindVulkan creates.
find_path(_saige_vulkan_include vulkan/vulkan_core.h
    HINTS "$ENV{VULKAN_SDK}/include" "$ENV{VULKAN_SDK}/Include" NO_CACHE)
set(_saige_system_header_version 0)
if(_saige_vulkan_include)
    file(STRINGS "${_saige_vulkan_include}/vulkan/vulkan_core.h" _saige_version_line REGEX "^#define VK_HEADER_VERSION [0-9]+")
    if(_saige_version_line MATCHES "VK_HEADER_VERSION ([0-9]+)")
        set(_saige_system_header_version ${CMAKE_MATCH_1})
    endif()
endif()
set(_saige_system_headers_ok FALSE)
if(_saige_system_header_version GREATER_EQUAL SAIGE_VULKAN_MIN_HEADER_VERSION)
    find_package(Vulkan QUIET)
    if(TARGET Vulkan::Headers)
        set(_saige_system_headers_ok TRUE)
    endif()
elseif(_saige_vulkan_include)
    message(STATUS "Installed Vulkan headers are 1.x.${_saige_system_header_version}; "
                   "1.4.${SAIGE_VULKAN_MIN_HEADER_VERSION} or newer is required")
endif()
if(_saige_system_headers_ok)
    set(_saige_vulkan_headers Vulkan::Headers)
elseif(SAIGE_VULKAN_FETCH_HEADERS)
    include(FetchContent)
    FetchContent_Declare(saige_vulkan_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG ${SAIGE_VULKAN_HEADERS_TAG}
        GIT_SHALLOW TRUE)
    # Only the C headers are needed; skip Vulkan-Headers' optional C++20 module (vulkan.cppm), which slows MSVC builds.
    set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(saige_vulkan_headers)
    # Third-party headers are not held to this project's warning level: expose them as system includes, as an installed SDK's
    # would be. (Their version macros use C-style casts, which -Wold-style-cast -Werror rejects in our code otherwise.)
    get_target_property(_saige_fetched_include Vulkan-Headers INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(Vulkan-Headers PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_saige_fetched_include}")
    set(_saige_vulkan_headers Vulkan::Headers)
else()
    message(FATAL_ERROR "SAIGE_ENABLE_VULKAN needs Vulkan headers 1.4.${SAIGE_VULKAN_MIN_HEADER_VERSION} or newer: install a "
                        "current Vulkan SDK, or configure with -DSAIGE_VULKAN_FETCH_HEADERS=ON. (Building against newer "
                        "headers does not raise the runtime requirement: Vulkan 1.3 remains the baseline.)")
endif()

add_library(saige_rhi_vulkan STATIC
    src/vk_api.cpp
    src/vulkan_device.cpp
)
add_library(saige::rhi_vulkan ALIAS saige_rhi_vulkan)
target_include_directories(saige_rhi_vulkan PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(saige_rhi_vulkan PUBLIC saige::rhi PRIVATE ${_saige_vulkan_headers} ${CMAKE_DL_LIBS})
saige_gfx_configure_target(saige_rhi_vulkan)
if(NOT MSVC)
    # Vulkan structs are initialized as `VkFoo info{VK_STRUCTURE_TYPE_FOO};` (remaining members are
    # value-initialized to zero by the language), which this warning flags on every struct.
    target_compile_options(saige_rhi_vulkan PRIVATE -Wno-missing-field-initializers)
endif()

# Linux presentation through X11 (used by the Xvfb swapchain test; Core's 0.1 desktop host is Win32).
if(UNIX AND NOT APPLE)
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_compile_definitions(saige_rhi_vulkan PRIVATE SAIGE_VULKAN_XLIB)
        target_include_directories(saige_rhi_vulkan PRIVATE ${X11_INCLUDE_DIR})
        set(SAIGE_VULKAN_HAS_XLIB ON CACHE INTERNAL "")
    endif()
endif()

if(SAIGE_RHI_VULKAN_BUILD_TESTS)
    # rhi::testing (fault injection and the process-wide validation counter) exists only in builds that build the tests.
    target_compile_definitions(saige_rhi_vulkan PUBLIC SAIGE_RHI_VULKAN_TESTING)
    enable_testing()
    if(NOT TARGET saige::render)
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../render ${CMAKE_CURRENT_BINARY_DIR}/render)
    endif()
    # test_capabilities.cpp is last on purpose: its printed report (loader, instance and effective versions, and where each
    # capability comes from) then sits at the end of the verbose ctest log, inside the part CI log viewers retain.
    add_executable(saige_rhi_vulkan_tests tests/test_vulkan_device.cpp tests/test_scenes.cpp tests/test_texture_upload.cpp tests/test_bind_groups.cpp tests/test_gpu_resource_lifetime.cpp tests/test_models.cpp tests/test_diagnostics.cpp tests/test_capabilities.cpp)
    target_compile_definitions(saige_rhi_vulkan_tests PRIVATE SAIGE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/goldens")
    target_include_directories(saige_rhi_vulkan_tests PRIVATE ${SAIGE_GFX_SOURCE_ROOT}/engine/gfx_test_support/include
                                                              ${SAIGE_GFX_SOURCE_ROOT}/shaders/generated)
    target_link_libraries(saige_rhi_vulkan_tests PRIVATE saige::rhi_vulkan saige::render)
    if(SAIGE_VULKAN_HAS_XLIB)
        target_sources(saige_rhi_vulkan_tests PRIVATE tests/x11_window.cpp)
        target_compile_definitions(saige_rhi_vulkan_tests PRIVATE SAIGE_TEST_XLIB)
        target_link_libraries(saige_rhi_vulkan_tests PRIVATE ${X11_LIBRARIES})
        target_include_directories(saige_rhi_vulkan_tests PRIVATE ${X11_INCLUDE_DIR})
    endif()
    saige_gfx_configure_target(saige_rhi_vulkan_tests)
    add_test(NAME saige_rhi_vulkan_tests COMMAND saige_rhi_vulkan_tests)
    # Exit code 77 = no Vulkan 1.3 device on this machine (e.g. CI without lavapipe): reported as skipped.
    set_tests_properties(saige_rhi_vulkan_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()
