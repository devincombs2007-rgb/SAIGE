# saige::gfx_assets: graphics asset import (glTF 2.0 to CPU mesh/material data). No external dependencies.
# docs/rendering/gltf-subset.md defines exactly what is supported.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_gfx_assets LANGUAGES CXX)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_GFX_ASSETS_BUILD_TESTS "Build saige::gfx_assets tests" ${_saige_tests_default})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)
if(NOT TARGET saige::render)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../render ${CMAKE_CURRENT_BINARY_DIR}/render)
endif()

add_library(saige_gfx_assets STATIC
    src/gltf.cpp
    src/json.cpp
    src/resource_uri.cpp
)
add_library(saige::gfx_assets ALIAS saige_gfx_assets)
target_include_directories(saige_gfx_assets PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(saige_gfx_assets PUBLIC saige::render)  # math and transform types
saige_gfx_configure_target(saige_gfx_assets)

if(SAIGE_GFX_ASSETS_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_gfx_assets_tests
        SOURCES
            tests/test_gltf.cpp
            tests/test_json.cpp
            tests/test_resource_uri.cpp
        LINK saige::gfx_assets)
    target_compile_definitions(saige_gfx_assets_tests PRIVATE SAIGE_GLTF_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()
