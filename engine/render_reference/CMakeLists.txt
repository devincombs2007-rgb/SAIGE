# saige::render_reference: CPU reference implementations of render-pass algorithms, so a shader port has an oracle and the algorithm is tested without a GPU. Entries: temporal anti-aliasing (jitter sequence,
# reprojection, YCoCg neighbourhood clipping, history blend) and local lights (point and spot: photometric units, windowed inverse-square falloff, spot cone). No dependencies. Builds standalone: cmake -S engine/render_reference -B build-rr. These are correctness references, never performance evidence.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_render_reference LANGUAGES CXX)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_RENDER_REFERENCE_BUILD_TESTS "Build saige::render_reference tests" ${_saige_tests_default})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)

add_library(saige_render_reference STATIC src/taa.cpp src/lights.cpp)
add_library(saige::render_reference ALIAS saige_render_reference)
target_include_directories(saige_render_reference PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
saige_gfx_configure_target(saige_render_reference)

if(SAIGE_RENDER_REFERENCE_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_render_reference_tests SOURCES tests/test_taa.cpp tests/test_lights.cpp LINK saige::render_reference)
endif()
