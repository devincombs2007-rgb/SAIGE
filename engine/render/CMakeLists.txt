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
if(NOT TARGET saige::rhi_null)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../rhi_null ${CMAKE_CURRENT_BINARY_DIR}/rhi_null)
endif()

add_library(saige_render STATIC
    src/camera.cpp
    src/env_brdf.cpp
    src/env_brdf_asset.cpp
    src/gpu_resources.cpp
    src/model_renderer.cpp
    src/render_graph.cpp
    src/scene_renderer.cpp
    src/transform.cpp
)
add_library(saige::render ALIAS saige_render)

target_include_directories(saige_render PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)
# Embedded SPIR-V generated from shaders/ (see shaders/CMakeLists.txt).
target_include_directories(saige_render PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../../shaders/generated)
target_link_libraries(saige_render PUBLIC saige::rhi)
saige_gfx_configure_target(saige_render)

# Generator and checker of the embedded environment BRDF table (engine/render/tools/envbrdf_gen.cpp). Regenerate with
# `cmake --build <dir> --target saige_regenerate_env_brdf` after changing the BRDF terms (and bump kEnvBrdfTableVersion).
add_executable(saige_envbrdf_gen tools/envbrdf_gen.cpp)
target_link_libraries(saige_envbrdf_gen PRIVATE saige_render)
saige_gfx_configure_target(saige_envbrdf_gen)
add_custom_target(saige_regenerate_env_brdf
    COMMAND saige_envbrdf_gen generate ${CMAKE_CURRENT_SOURCE_DIR}/src/generated/env_brdf_table.inc
    COMMENT "Regenerating the embedded environment BRDF table (about 7 s at -O2)"
    VERBATIM)

if(SAIGE_RENDER_BUILD_TESTS)
    add_test(NAME saige_env_brdf_table_up_to_date COMMAND saige_envbrdf_gen verify)
    set_tests_properties(saige_env_brdf_table_up_to_date PROPERTIES TIMEOUT 120)
    # The registry's fault-injection hook (render::testing) exists only in builds that build the tests.
    target_compile_definitions(saige_render PUBLIC SAIGE_RENDER_TESTING)
    enable_testing()
    saige_gfx_add_test(saige_render_tests
        SOURCES
            tests/test_camera.cpp
            tests/test_gpu_resources.cpp
            tests/test_math.cpp
            tests/test_render_graph.cpp
            tests/test_scene_renderer.cpp
            tests/test_transform.cpp
        LINK saige::render saige::rhi_null
    )
endif()
