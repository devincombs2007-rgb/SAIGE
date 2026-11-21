# saige::rhi_null: CPU-only RHI backend for tests and headless runs (no GPU, no external dependencies).
cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_rhi_null LANGUAGES CXX)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_RHI_NULL_BUILD_TESTS "Build saige::rhi_null tests" ${_saige_tests_default})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)
if(NOT TARGET saige::rhi)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../rhi ${CMAKE_CURRENT_BINARY_DIR}/rhi)
endif()

add_library(saige_rhi_null STATIC src/null_device.cpp)
add_library(saige::rhi_null ALIAS saige_rhi_null)
target_include_directories(saige_rhi_null PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(saige_rhi_null PUBLIC saige::rhi)
saige_gfx_configure_target(saige_rhi_null)

if(SAIGE_RHI_NULL_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_rhi_null_tests SOURCES tests/test_null_device.cpp LINK saige::rhi_null)
endif()
