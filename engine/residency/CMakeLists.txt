# saige::residency: CPU accounting for streamed GPU resources. A ledger of resources with quality levels (texture mips, geometry LODs), per-category and total byte budgets, pinned resources, a per-frame upload
# allowance, and a deterministic planner that decides what to upgrade, degrade and evict as the demand changes. It allocates nothing on a device: a renderer asks for a plan and carries it out. No dependencies.
# Builds standalone: cmake -S engine/residency -B build-res.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_residency LANGUAGES CXX)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_RESIDENCY_BUILD_TESTS "Build saige::residency tests" ${_saige_tests_default})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)

add_library(saige_residency STATIC src/ledger.cpp)
add_library(saige::residency ALIAS saige_residency)
target_include_directories(saige_residency PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
saige_gfx_configure_target(saige_residency)

if(SAIGE_RESIDENCY_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_residency_tests SOURCES tests/test_ledger.cpp LINK saige::residency)
endif()
