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
# Reference MikkTSpace (zlib), pinned by upstream commit and SHA-256 (docs/third-party/mikktspace.md); fetched, not vendored. It is
# compiled as an isolated C target with warnings off for that target only. Offline builds set
# FETCHCONTENT_SOURCE_DIR_SAIGE_MIKKTSPACE_C and FETCHCONTENT_SOURCE_DIR_SAIGE_MIKKTSPACE_H to the directories holding the
# verified files. The Lengyel-style generator (src/tangents.cpp) remains the importer's default and the fallback.
option(SAIGE_GFX_MIKKTSPACE "Build the pinned reference MikkTSpace and generate_tangents_mikktspace" ON)
if(SAIGE_GFX_MIKKTSPACE)
    enable_language(C)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    set(SAIGE_MIKKTSPACE_COMMIT 3e895b49d05ea07e4c2133156cfa94369e19e409)
    FetchContent_Declare(saige_mikktspace_c
        URL https://raw.githubusercontent.com/mmikk/MikkTSpace/${SAIGE_MIKKTSPACE_COMMIT}/mikktspace.c
        URL_HASH SHA256=de87e74107df766ce68108801262bd8d53899414236b59810509a8fc2a51e288
        DOWNLOAD_NO_EXTRACT TRUE)
    FetchContent_Declare(saige_mikktspace_h
        URL https://raw.githubusercontent.com/mmikk/MikkTSpace/${SAIGE_MIKKTSPACE_COMMIT}/mikktspace.h
        URL_HASH SHA256=17fc433894f24c73753d548086cc4d8c5c0379f4a6edfb98b5da243e4f0bc3d0
        DOWNLOAD_NO_EXTRACT TRUE)
    FetchContent_MakeAvailable(saige_mikktspace_c saige_mikktspace_h)
    add_library(saige_mikktspace STATIC ${saige_mikktspace_c_SOURCE_DIR}/mikktspace.c)
    target_include_directories(saige_mikktspace SYSTEM PUBLIC ${saige_mikktspace_h_SOURCE_DIR})
    target_compile_options(saige_mikktspace PRIVATE $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)  # third-party C: no warnings from it
    set_target_properties(saige_mikktspace PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_sources(saige_gfx_assets PRIVATE src/tangents_mikktspace.cpp)
    target_link_libraries(saige_gfx_assets PRIVATE saige_mikktspace)
    target_compile_definitions(saige_gfx_assets PUBLIC SAIGE_HAS_MIKKTSPACE=1)
endif()
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
            tests/test_tangents.cpp
        LINK saige::gfx_assets)
    if(SAIGE_GFX_MIKKTSPACE)
        target_sources(saige_gfx_assets_tests PRIVATE tests/test_tangents_mikktspace.cpp)
    endif()
    if(SAIGE_GFX_IMAGE_DECODE)
        target_sources(saige_gfx_assets_tests PRIVATE tests/test_image.cpp)
        target_compile_definitions(saige_gfx_assets_tests PRIVATE
            SAIGE_TEXTURED_SCENE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../examples/textured-scene")
    endif()
    target_compile_definitions(saige_gfx_assets_tests PRIVATE SAIGE_GLTF_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()
