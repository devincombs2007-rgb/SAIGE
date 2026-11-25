# saige::animation: reusable character animation on top of gfx_assets' stateless sampling: a humanoid bone map, pose blending, clip analysis, a locomotion state machine with
# stride-matched playback, retargeting, a BVH importer and IK. docs/animation-character.md has the ownership contract. Needs saige::gfx_assets; the controller glue also uses saige::core when present.
cmake_minimum_required(VERSION 3.21)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    project(saige_animation LANGUAGES CXX)
    set(_saige_tests_default ON)
else()
    set(_saige_tests_default ${BUILD_TESTING})
endif()
option(SAIGE_ANIMATION_BUILD_TESTS "Build saige::animation tests" ${_saige_tests_default})

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/SaigeGraphics.cmake)
if(NOT TARGET saige::gfx_assets)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../gfx_assets ${CMAKE_CURRENT_BINARY_DIR}/gfx_assets)
endif()

add_library(saige_animation STATIC
    src/bvh.cpp
    src/action.cpp
    src/clip_analysis.cpp
    src/humanoid.cpp
    src/ik.cpp
    src/layers.cpp
    src/locomotion.cpp
    src/pose_ops.cpp
    src/retarget.cpp
    src/seat_entry.cpp
)
add_library(saige::animation ALIAS saige_animation)
target_include_directories(saige_animation PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(saige_animation PUBLIC saige::gfx_assets)
saige_gfx_configure_target(saige_animation)

add_executable(saige_anim_inspect tools/anim_inspect.cpp)
target_link_libraries(saige_anim_inspect PRIVATE saige::animation)
saige_gfx_configure_target(saige_anim_inspect)

add_executable(saige_anim_mocap_probe tools/mocap_probe.cpp)
target_link_libraries(saige_anim_mocap_probe PRIVATE saige::animation)
saige_gfx_configure_target(saige_anim_mocap_probe)

if(SAIGE_ANIMATION_BUILD_TESTS)
    enable_testing()
    saige_gfx_add_test(saige_animation_tests
        SOURCES
            tests/test_humanoid.cpp
            tests/test_action.cpp
            tests/test_ik.cpp
            tests/test_seat_entry.cpp
            tests/test_layers.cpp
            tests/test_locomotion.cpp
            tests/test_retarget.cpp
        LINK saige::animation)
    target_include_directories(saige_animation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    # The module on real assets (UAL rig and clips, CMU motion capture): skipped with exit 77 until tools/animation/fetch_humanoid_assets.py has filled a directory (SAIGE_ANIM_ASSETS).
    add_executable(saige_animation_assets_test tests/assets_test.cpp)
    target_include_directories(saige_animation_assets_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(saige_animation_assets_test PRIVATE saige::animation)
    saige_gfx_configure_target(saige_animation_assets_test)
    add_test(NAME saige_animation_assets COMMAND saige_animation_assets_test)
    set_tests_properties(saige_animation_assets PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    # The animation against the physical character (needs Core's capsule controller and static collision, so only in the root build).
    if(TARGET saige::core)
        saige_gfx_add_test(saige_animation_character_tests
            SOURCES tests/test_character.cpp
            LINK saige::animation saige::core)
        target_include_directories(saige_animation_character_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    endif()
endif()

# A runnable example of the chain (capsule character, locomotion, ground and wall IK); needs Core.
if(TARGET saige::core)
    add_executable(saige_anim_character_demo tools/character_demo.cpp)
    target_link_libraries(saige_anim_character_demo PRIVATE saige::animation saige::core)
    saige_gfx_configure_target(saige_anim_character_demo)
endif()
