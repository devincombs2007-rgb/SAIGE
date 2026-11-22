# Test support for the graphics modules: the mini test harness (saige_test.hpp) and image comparison (image.hpp).
# Header-only; this directory builds a test executable for the support code itself, so a bug in the comparison logic
# cannot silently hide a rendering regression.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_gfx_test_support LANGUAGES CXX)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_GFX_TEST_SUPPORT_BUILD_TESTS "Build tests of the test support code" ${_saige_tests_default})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)

if(SAIGE_GFX_TEST_SUPPORT_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_gfx_test_support_tests SOURCES tests/test_image.cpp tests/test_ibl_reference.cpp)
    # The IBL reference uses the renderer's header-only math (Float3).
    target_include_directories(saige_gfx_test_support_tests PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../render/include)
endif()
