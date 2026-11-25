cmake_minimum_required(VERSION 3.21)
project(SAIGE VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

add_library(saige_core STATIC
    src/asset_id.cpp
    src/capsule_character.cpp
    src/capsule_sweep.cpp
    src/free_camera.cpp
    src/runtime.cpp
    src/scene_hierarchy.cpp
    src/world.cpp
)
add_library(saige::core ALIAS saige_core)
target_include_directories(saige_core PUBLIC include)
target_compile_features(saige_core PUBLIC cxx_std_20)

add_executable(saige_headless apps/headless.cpp)
target_link_libraries(saige_headless PRIVATE saige_core)

if(WIN32)
    add_library(saige_win32 STATIC src/win32_platform.cpp)
    add_library(saige::desktop ALIAS saige_win32)
    target_link_libraries(saige_win32 PUBLIC saige::core PRIVATE user32)
    target_compile_definitions(saige_win32 PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00)

    add_executable(saige_desktop apps/desktop.cpp)
    target_link_libraries(saige_desktop PRIVATE saige::desktop)
endif()

include(CTest)
if(BUILD_TESTING)
    foreach(suite IN ITEMS world runtime scene_hierarchy asset_id free_camera capsule_sweep capsule_character)
        add_executable(${suite}_test tests/${suite}_test.cpp)
        target_link_libraries(${suite}_test PRIVATE saige_core)
        add_test(NAME ${suite} COMMAND ${suite}_test)
    endforeach()

    add_test(NAME headless_boot COMMAND saige_headless --frames 3)
    set_tests_properties(headless_boot PROPERTIES
        PASS_REGULAR_EXPRESSION "frames=3([^0-9]|$)"
        TIMEOUT 10
    )

    if(NOT CMAKE_CROSSCOMPILING)
        find_package(Python3 COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME agent_cli COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_cli_test.py" $<TARGET_FILE:saige_headless>)
        endif()
    endif()

    if(WIN32)
        add_executable(win32_platform_test tests/win32_platform_test.cpp)
        target_link_libraries(win32_platform_test PRIVATE saige::desktop user32)
        target_compile_definitions(win32_platform_test PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
        add_test(NAME win32_platform COMMAND win32_platform_test)
        add_test(NAME desktop_boot COMMAND saige_desktop --frames 3)
        set_tests_properties(win32_platform desktop_boot PROPERTIES TIMEOUT 30)
    endif()
endif()

# Graphics modules (owner: Rendering; docs/rendering). Additive: the core and desktop targets do not
# depend on them. Vulkan stays optional (SAIGE_ENABLE_VULKAN, default OFF; see engine/rhi_vulkan).
option(SAIGE_BUILD_GRAPHICS "Build the backend-agnostic graphics modules and shader tool" ON)
if(SAIGE_BUILD_GRAPHICS)
    add_subdirectory(tools/shaderc)
    add_subdirectory(shaders)
    add_subdirectory(engine)
endif()

option(SAIGE_TEST_WIN32_VULKAN "Run strict Win32 Vulkan presentation tests (requires a driver and validation layer)" OFF)
if(SAIGE_TEST_WIN32_VULKAN)
    if(NOT WIN32 OR NOT BUILD_TESTING OR NOT TARGET saige::rhi_vulkan OR NOT TARGET saige::render_sink)
        message(FATAL_ERROR "SAIGE_TEST_WIN32_VULKAN requires Windows, CTest, and graphics with Vulkan enabled")
    endif()
    add_executable(win32_vulkan_test tests/win32_vulkan_test.cpp)
    target_link_libraries(win32_vulkan_test PRIVATE saige::desktop saige::rhi_vulkan saige::render_sink user32)
    add_test(NAME win32_vulkan_lifecycle COMMAND win32_vulkan_test)
    set_tests_properties(win32_vulkan_lifecycle PROPERTIES TIMEOUT 60)
endif()

option(SAIGE_BUILD_PROJECTS "Build the project format and tools" ON)
if(SAIGE_BUILD_PROJECTS)
    include(FetchContent)
    set(JSON_BuildTests OFF CACHE INTERNAL "")
    set(JSON_Install OFF CACHE INTERNAL "")
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(saige_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
    FetchContent_MakeAvailable(saige_json)
    add_library(saige_project STATIC src/project.cpp src/project_session.cpp)
    add_library(saige::project ALIAS saige_project)
    target_link_libraries(saige_project PUBLIC saige::core PRIVATE nlohmann_json::nlohmann_json)
    add_executable(saige_scene_trace apps/scene_trace.cpp)
    target_link_libraries(saige_scene_trace PRIVATE saige::project)
    add_executable(saige_project_tool apps/project_tool.cpp)
    target_link_libraries(saige_project_tool PRIVATE saige::project)
    if(BUILD_TESTING)
        add_executable(project_test tests/project_test.cpp)
        target_link_libraries(project_test PRIVATE saige::project)
        add_test(NAME project_round_trip COMMAND project_test)
        add_executable(project_session_test tests/project_session_test.cpp)
        target_link_libraries(project_session_test PRIVATE saige::project)
        add_test(NAME project_runtime_session COMMAND project_session_test
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured-scene/saige.scene.json")
    endif()
endif()

if(TARGET saige::project AND TARGET saige::gfx_assets)
    add_library(saige_scene_project STATIC src/chunk_manifest.cpp src/chunk_stream.cpp src/prepared_scene.cpp)
    add_library(saige::scene_project ALIAS saige_scene_project)
    target_link_libraries(saige_scene_project PUBLIC saige::project saige::gfx_assets PRIVATE nlohmann_json::nlohmann_json)
    add_executable(saige_scene_asset_tool apps/scene_asset_tool.cpp)
    target_link_libraries(saige_scene_asset_tool PRIVATE saige::scene_project)
    if(BUILD_TESTING)
        add_executable(chunk_stream_test tests/chunk_stream_test.cpp)
        target_link_libraries(chunk_stream_test PRIVATE saige::scene_project)
        add_test(NAME chunk_stream_staging COMMAND chunk_stream_test
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured-scene/saige.scene.json")
        add_executable(prepared_scene_test tests/prepared_scene_test.cpp)
        target_link_libraries(prepared_scene_test PRIVATE saige::scene_project)
        add_test(NAME prepared_scene_replacement COMMAND prepared_scene_test
            "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured-scene/saige.scene.json")
        if(WIN32)
            add_executable(win32_prepared_scene_test tests/win32_prepared_scene_test.cpp)
            target_link_libraries(win32_prepared_scene_test PRIVATE saige::desktop saige::scene_project user32)
            add_test(NAME win32_prepared_scene COMMAND win32_prepared_scene_test
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured-scene/saige.scene.json")
            set_tests_properties(win32_prepared_scene PROPERTIES TIMEOUT 30)
        endif()
        find_package(Python3 COMPONENTS Interpreter)
        if(Python3_Interpreter_FOUND)
            add_test(NAME scene_asset_preflight COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/scene_asset_preflight_test.py"
                $<TARGET_FILE:saige_scene_asset_tool> $<TARGET_FILE:saige_scene_trace>)
        endif()
    endif()
endif()
