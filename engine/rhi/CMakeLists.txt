# saige::rhi — backend-agnostic Render Hardware Interface.
#
# Builds standalone (`cmake -S engine/rhi -B build/rhi`) or as part of the engine via
# `add_subdirectory(engine/rhi)` from Core's root CMakeLists (issue #2, D-02).

cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_rhi LANGUAGES CXX)
    set(SAIGE_RHI_STANDALONE ON)
else()
    set(SAIGE_RHI_STANDALONE OFF)
endif()

if(SAIGE_RHI_STANDALONE)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_RHI_BUILD_TESTS "Build saige::rhi unit tests" ${_saige_tests_default})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)

add_library(saige_rhi STATIC
    src/bindless_allocator.cpp
    src/blend.cpp
    src/resource_desc.cpp
)
add_library(saige::rhi ALIAS saige_rhi)

target_include_directories(saige_rhi PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
saige_gfx_configure_target(saige_rhi)

if(SAIGE_RHI_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_rhi_tests
        SOURCES
            tests/test_barrier.cpp
            tests/test_blend.cpp
            tests/test_bindless_allocator.cpp
            tests/test_format.cpp
            tests/test_handle.cpp
            tests/test_resource_desc.cpp
        LINK saige::rhi
    )
endif()
