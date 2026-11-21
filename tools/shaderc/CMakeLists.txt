# saige-shaderc — offline shader tool (docs/rendering/shader-pipeline.md, issue #4).
#
# saige_shaderc_core: annotation parsing, permutation expansion, include scanning and cache keys
#                     (no external dependencies).
# saige-shaderc:      CLI. DXC compilation is added once the third-party policy (issue #2, D-03) is decided.

cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_shaderc LANGUAGES CXX)
    set(SAIGE_SHADERC_STANDALONE ON)
else()
    set(SAIGE_SHADERC_STANDALONE OFF)
endif()

if(SAIGE_SHADERC_STANDALONE)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_SHADERC_BUILD_TESTS "Build saige-shaderc tests" ${_saige_tests_default})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)

add_library(saige_shaderc_core STATIC
    src/annotations.cpp
    src/cache_key.cpp
    src/compile.cpp
    src/includes.cpp
    src/permutations.cpp
)
add_library(saige::shaderc_core ALIAS saige_shaderc_core)
target_include_directories(saige_shaderc_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
saige_gfx_configure_target(saige_shaderc_core)

add_executable(saige-shaderc src/main.cpp)
target_link_libraries(saige-shaderc PRIVATE saige::shaderc_core)
saige_gfx_configure_target(saige-shaderc)

if(SAIGE_SHADERC_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_shaderc_tests
        SOURCES
            tests/test_annotations.cpp
            tests/test_cache_key.cpp
            tests/test_compile.cpp
            tests/test_includes.cpp
            tests/test_permutations.cpp
        LINK saige::shaderc_core
    )

    # CLI smoke tests on a real fixture file.
    set(_fixture ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/gbuffer_example.hlsl)
    add_test(NAME saige_shaderc_cli_check COMMAND saige-shaderc check ${_fixture})
    add_test(NAME saige_shaderc_cli_permutations COMMAND saige-shaderc permutations ${_fixture})
    add_test(NAME saige_shaderc_cli_deps
             COMMAND saige-shaderc deps ${_fixture} -I ${CMAKE_CURRENT_SOURCE_DIR}/tests/data/include)
    set_tests_properties(saige_shaderc_cli_deps PROPERTIES PASS_REGULAR_EXPRESSION "common/bindless.hlsli")
    set_tests_properties(saige_shaderc_cli_check PROPERTIES PASS_REGULAR_EXPRESSION "GBufferExample: OK")
endif()
