# saige::rhi_vulkan: Vulkan RHI backend (docs/rendering/rhi-design.md). Vulkan 1.3 is the required runtime baseline;
# newer API versions are negotiated at run time (docs/rendering/vulkan-adapter-matrix.md).
#
# Only the Vulkan *headers* are needed at build time: the loader (vulkan-1.dll / libvulkan.so.1) is opened at
# runtime, so builds have no link dependency on the Vulkan SDK. Header sources, in order:
#   1. Installed headers (Vulkan SDK or a distro package such as libvulkan-dev), if a translation unit including the very directory that
#      will be used compiles with the Vulkan 1.4 core API (cmake/SaigeVulkanHeaders.cmake); older ones are skipped
#   2. SAIGE_VULKAN_FETCH_HEADERS=ON: Khronos Vulkan-Headers at the pinned SDK release commit via FetchContent (Apache-2.0), checked the
#      same way
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
# The SDK-aligned release fetched when no sufficiently new headers are installed, pinned to the full commit (the tag
# vulkan-sdk-1.4.363.0 resolves to it, but a tag can move; a commit cannot). Offline builds can set
# FETCHCONTENT_SOURCE_DIR_SAIGE_VULKAN_HEADERS to a checkout of that commit.
set(SAIGE_VULKAN_HEADERS_COMMIT 6802bb4733b63ed5efd3adb308a6c885ef180ea1)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/SaigeVulkanHeaders.cmake)

# Installed headers: the directory a build would be given (a user-supplied Vulkan_INCLUDE_DIR, the SDK, CMAKE_PREFIX_PATH or the system
# paths). They are used only if a translation unit including *that directory* compiles with the 1.4 API: the tree that was inspected is the
# tree that is used. (find_package(Vulkan) is deliberately not used: it could select a different tree than the one probed, and the
# fetched Vulkan-Headers defines its own Vulkan::Headers target that cannot coexist with the one FindVulkan creates.)
find_path(_saige_vulkan_include vulkan/vulkan_core.h
    HINTS "${Vulkan_INCLUDE_DIR}" "$ENV{VULKAN_SDK}/include" "$ENV{VULKAN_SDK}/Include" NO_CACHE)
set(_saige_system_headers_ok FALSE)
if(_saige_vulkan_include)
    saige_vulkan_headers_usable("${_saige_vulkan_include}" _saige_system_headers_ok)
    if(NOT _saige_system_headers_ok)
        message(STATUS "Installed Vulkan headers in ${_saige_vulkan_include} do not provide the Vulkan 1.4 API "
                       "(need 1.4.${SAIGE_VULKAN_MIN_HEADER_VERSION} or newer)")
    endif()
endif()
if(_saige_system_headers_ok)
    add_library(saige_vulkan_headers INTERFACE)
    target_include_directories(saige_vulkan_headers SYSTEM INTERFACE "${_saige_vulkan_include}")
    set(_saige_vulkan_headers saige_vulkan_headers)
elseif(SAIGE_VULKAN_FETCH_HEADERS)
    include(FetchContent)
    FetchContent_Declare(saige_vulkan_headers_src
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG ${SAIGE_VULKAN_HEADERS_COMMIT}
        GIT_SHALLOW FALSE)  # a shallow fetch needs a branch or tag; the commit pin is what makes the build reproducible
    # Only the C headers are needed; skip Vulkan-Headers' optional C++20 module (vulkan.cppm), which slows MSVC builds.
    set(VULKAN_HEADERS_ENABLE_MODULE OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(saige_vulkan_headers_src)
    # Third-party headers are not held to this project's warning level: expose them as system includes, as an installed SDK's
    # would be. (Their version macros use C-style casts, which -Wold-style-cast -Werror rejects in our code otherwise.)
    set(_saige_fetched_include "${saige_vulkan_headers_src_SOURCE_DIR}/include")
    saige_vulkan_headers_usable("${_saige_fetched_include}" _saige_fetched_ok)
    if(NOT _saige_fetched_ok)
        message(FATAL_ERROR "The fetched Vulkan-Headers (${SAIGE_VULKAN_HEADERS_COMMIT}) do not provide the Vulkan 1.4 API; see "
                            "${CMAKE_BINARY_DIR}/saige_vulkan_header_probe for the compiler output")
    endif()
    add_library(saige_vulkan_headers INTERFACE)
    target_include_directories(saige_vulkan_headers SYSTEM INTERFACE "${_saige_fetched_include}")
    set(_saige_vulkan_headers saige_vulkan_headers)
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
    # Header selection: the compile probe must refuse headers that predate 1.4 and headers whose patch number alone looks new enough,
    # and accept a set that declares the 1.4 API.
    foreach(_case old patch_only good)
        add_test(NAME saige_vulkan_header_selection_${_case}
            COMMAND ${CMAKE_COMMAND} -S ${CMAKE_CURRENT_LIST_DIR}/tests/cmake/header_selection
                    -B ${CMAKE_CURRENT_BINARY_DIR}/header_selection_${_case}
                    -DFAKE_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/tests/cmake/header_selection/fake_${_case}
                    -DSAIGE_VULKAN_HEADERS_MODULE=${CMAKE_CURRENT_LIST_DIR}/cmake/SaigeVulkanHeaders.cmake)
    endforeach()
    set_tests_properties(saige_vulkan_header_selection_old saige_vulkan_header_selection_patch_only PROPERTIES
        PASS_REGULAR_EXPRESSION "HEADERS_USABLE=FALSE")
    set_tests_properties(saige_vulkan_header_selection_good PROPERTIES PASS_REGULAR_EXPRESSION "HEADERS_USABLE=TRUE")
    if(NOT TARGET saige::render)
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../render ${CMAKE_CURRENT_BINARY_DIR}/render)
    endif()
    # test_capabilities.cpp is last on purpose: its printed report (loader, instance and effective versions, and where each
    # capability comes from) then sits at the end of the verbose ctest log, inside the part CI log viewers retain.
    if(NOT TARGET saige::gfx_assets)
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../gfx_assets ${CMAKE_CURRENT_BINARY_DIR}/gfx_assets)
    endif()
    if(NOT TARGET saige::gfx_scene)
        add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../gfx_scene ${CMAKE_CURRENT_BINARY_DIR}/gfx_scene)
    endif()
    add_executable(saige_rhi_vulkan_tests tests/test_vulkan_device.cpp tests/test_scenes.cpp tests/test_texture_upload.cpp tests/test_bind_groups.cpp tests/test_gpu_resource_lifetime.cpp tests/test_models.cpp tests/test_pbr.cpp tests/test_hdr.cpp tests/test_blend.cpp tests/test_blend_render.cpp tests/test_diagnostics.cpp tests/test_tonemap.cpp tests/test_capabilities.cpp tests/test_shadow_map.cpp tests/test_frustum_culling.cpp tests/test_bloom.cpp tests/test_fxaa.cpp tests/test_ambient_occlusion.cpp tests/test_sun_shadows.cpp tests/test_shadow_cascades.cpp tests/test_ibl.cpp tests/test_skinning.cpp)
    target_compile_definitions(saige_rhi_vulkan_tests PRIVATE SAIGE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/goldens")
    target_include_directories(saige_rhi_vulkan_tests PRIVATE ${SAIGE_GFX_SOURCE_ROOT}/engine/gfx_test_support/include
                                                              ${SAIGE_GFX_SOURCE_ROOT}/shaders/generated)
    target_link_libraries(saige_rhi_vulkan_tests PRIVATE saige::rhi_vulkan saige::render saige::gfx_assets saige::gfx_scene)  # gfx_assets: the CPU environment precompute that feeds the IBL tests
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
