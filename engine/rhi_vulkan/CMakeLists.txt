# saige::rhi_vulkan: Vulkan 1.3 RHI backend (docs/rendering/rhi-design.md).
#
# Only the Vulkan *headers* are needed at build time: the loader (vulkan-1.dll / libvulkan.so.1) is opened at
# runtime, so builds have no link dependency on the Vulkan SDK. Header sources, in order:
#   1. find_package(Vulkan) (Vulkan SDK or distro package such as libvulkan-dev)
#   2. SAIGE_VULKAN_FETCH_HEADERS=ON: Khronos Vulkan-Headers v1.3.296 via FetchContent (Apache-2.0)
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

find_package(Vulkan QUIET)
if(TARGET Vulkan::Headers)
    set(_saige_vulkan_headers Vulkan::Headers)
elseif(SAIGE_VULKAN_FETCH_HEADERS)
    include(FetchContent)
    FetchContent_Declare(saige_vulkan_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG v1.3.296
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(saige_vulkan_headers)
    set(_saige_vulkan_headers Vulkan::Headers)
else()
    message(FATAL_ERROR "SAIGE_ENABLE_VULKAN needs the Vulkan headers: install the Vulkan SDK (or libvulkan-dev), "
                        "or configure with -DSAIGE_VULKAN_FETCH_HEADERS=ON.")
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
    enable_testing()
    if(NOT TARGET saige::render)
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../render ${CMAKE_CURRENT_BINARY_DIR}/render)
    endif()
    add_executable(saige_rhi_vulkan_tests tests/test_vulkan_device.cpp)
    target_include_directories(saige_rhi_vulkan_tests PRIVATE ${SAIGE_GFX_SOURCE_ROOT}/engine/gfx_test_support/include)
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
