# saige::render — renderer layer (render graph first; scene, passes and materials to follow).
#
# Builds standalone (`cmake -S engine/render -B build/render`, which pulls in engine/rhi) or via
# add_subdirectory from Core's root CMakeLists (issue #2, D-02).

cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_render LANGUAGES CXX)
    set(SAIGE_RENDER_STANDALONE ON)
else()
    set(SAIGE_RENDER_STANDALONE OFF)
endif()

if(SAIGE_RENDER_STANDALONE)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_RENDER_BUILD_TESTS "Build saige::render unit tests" ${_saige_tests_default})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)

if(NOT TARGET saige::rhi)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../rhi ${CMAKE_CURRENT_BINARY_DIR}/rhi)
endif()

add_library(saige_render STATIC
    src/render_graph.cpp
)
add_library(saige::render ALIAS saige_render)

target_include_directories(saige_render PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
target_link_libraries(saige_render PUBLIC saige::rhi)
saige_gfx_configure_target(saige_render)

if(SAIGE_RENDER_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_render_tests
        SOURCES
            tests/test_render_graph.cpp
        LINK saige::render
    )
endif()
