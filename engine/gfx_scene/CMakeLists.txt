# saige::gfx_scene: stages imported glTF assets (CPU data from gfx_assets) onto the GPU for the model renderer.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_gfx_scene LANGUAGES CXX)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_GFX_SCENE_BUILD_TESTS "Build saige::gfx_scene tests" ${_saige_tests_default})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)
if(NOT TARGET saige::render)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../render ${CMAKE_CURRENT_BINARY_DIR}/render)
endif()
if(NOT TARGET saige::gfx_assets)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../gfx_assets ${CMAKE_CURRENT_BINARY_DIR}/gfx_assets)
endif()

add_library(saige_gfx_scene STATIC src/stage_model.cpp src/prepared_model.cpp)
add_library(saige::gfx_scene ALIAS saige_gfx_scene)
target_include_directories(saige_gfx_scene PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(saige_gfx_scene PUBLIC saige::render saige::gfx_assets)
saige_gfx_configure_target(saige_gfx_scene)

if(SAIGE_GFX_SCENE_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_gfx_scene_tests
        SOURCES tests/test_stage_model.cpp
        LINK saige::gfx_scene saige::rhi_null)
    target_compile_definitions(saige_gfx_scene_tests PRIVATE
        SAIGE_TEXTURED_SCENE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../examples/textured-scene")
endif()
