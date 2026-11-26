# saige::environment: reusable, deterministic, CPU-only environment models that a renderer or a world host consumes: weather (rain, cloud, wind, fog, storm transitions), surface wetness and puddles,
# wind-driven vegetation bend, stable grass scatter with LOD thinning, and boat wake foam. No GPU, window-system or physics dependency; every function is pure given its inputs and seed.
# docs/engineering/fidelity-plan.md has the ownership notes. Builds standalone: cmake -S engine/environment -B build-env.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_environment LANGUAGES CXX)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_ENVIRONMENT_BUILD_TESTS "Build saige::environment tests" ${_saige_tests_default})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)

add_library(saige_environment STATIC
    src/wind.cpp
    src/weather.cpp
    src/surface.cpp
    src/vegetation.cpp
    src/wake.cpp
)
add_library(saige::environment ALIAS saige_environment)
target_include_directories(saige_environment PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
saige_gfx_configure_target(saige_environment)

if(SAIGE_ENVIRONMENT_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_environment_tests
        SOURCES
            tests/test_weather.cpp
            tests/test_surface.cpp
            tests/test_vegetation.cpp
            tests/test_wake.cpp
        LINK saige::environment)
endif()
