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
    src/animation.cpp
    src/environment.cpp
    src/gltf.cpp
    src/hdr_image.cpp
    src/json.cpp
    src/resource_uri.cpp
    src/sky_environment.cpp
    src/tangents.cpp
)

# PNG/JPEG decoding with stb_image, pinned by upstream commit and SHA-256 (docs/third-party/stb_image.md). Offline builds
# can set FETCHCONTENT_SOURCE_DIR_SAIGE_STB_IMAGE to a directory holding the verified stb_image.h.
option(SAIGE_GFX_IMAGE_DECODE "Decode PNG/JPEG with the pinned stb_image" ON)
if(SAIGE_GFX_IMAGE_DECODE)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(saige_stb_image
        URL https://raw.githubusercontent.com/nothings/stb/6b5799bae8a1fe3792691ef4b72ae26434c3e799/stb_image.h
        URL_HASH SHA256=dfc2aadda6295f67f677587773f25e5bf8c8ad427ed0db7c518525c91e72e020
        DOWNLOAD_NO_EXTRACT TRUE)
    FetchContent_MakeAvailable(saige_stb_image)
    target_sources(saige_gfx_assets PRIVATE src/image.cpp)
    target_include_directories(saige_gfx_assets SYSTEM PRIVATE ${saige_stb_image_SOURCE_DIR})
    target_compile_definitions(saige_gfx_assets PUBLIC SAIGE_HAS_IMAGE_DECODE=1)
endif()
add_library(saige::gfx_assets ALIAS saige_gfx_assets)
target_include_directories(saige_gfx_assets PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(saige_gfx_assets PUBLIC saige::render)  # math and transform types
saige_gfx_configure_target(saige_gfx_assets)

if(SAIGE_GFX_ASSETS_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_gfx_assets_tests
        SOURCES
            tests/alloc_fault.cpp
            tests/test_environment.cpp
            tests/test_gltf.cpp
            tests/test_gltf_skin.cpp
            tests/test_hdr_image.cpp
            tests/test_json.cpp
            tests/test_resource_uri.cpp
            tests/test_sky_environment.cpp
            tests/test_tangents.cpp
        LINK saige::gfx_assets)
    if(SAIGE_GFX_IMAGE_DECODE)
        target_sources(saige_gfx_assets_tests PRIVATE tests/test_image.cpp)
        target_compile_definitions(saige_gfx_assets_tests PRIVATE
            SAIGE_TEXTURED_SCENE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../examples/textured-scene")
    endif()
    target_compile_definitions(saige_gfx_assets_tests PRIVATE SAIGE_GLTF_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()
