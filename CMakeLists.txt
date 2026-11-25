cmake_minimum_required(VERSION 3.21)
project(SAIGE VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

add_library(saige_core STATIC
    src/asset_id.cpp
    src/capsule_character.cpp
    src/capsule_sweep.cpp
    src/capsule_triangle.cpp
    src/perf.cpp
    src/free_camera.cpp
    src/static_collision.cpp
    src/oriented_box.cpp
    src/wheel_contact.cpp
    src/water.cpp
    src/audio_model.cpp
    src/audio.cpp
    src/large_world.cpp
    src/navigation.cpp
    src/vehicle.cpp
    src/runtime.cpp
    src/scene_hierarchy.cpp
    src/world.cpp
)
add_library(saige::core ALIAS saige_core)
find_package(Threads REQUIRED)
target_link_libraries(saige_core PUBLIC Threads::Threads)
target_include_directories(saige_core PUBLIC include)
target_compile_features(saige_core PUBLIC cxx_std_20)

# Dynamic rigid-body backend (include/saige/dynamics.hpp). Optional until its compatibility gates pass in CI on every platform: with SAIGE_PHYSICS_JOLT=OFF (the default)
# nothing here is built and make_dynamics_world() returns null. SAIGE_JOLT_ROOT is a directory holding Jolt/ at the pinned release, fetched and hash-verified by
# tools/physics_eval/fetch_jolt.py (docs/physics-backends.md). SAIGE_JOLT_DOUBLE selects Jolt's double-precision build, evaluated separately from the default single precision.
option(SAIGE_PHYSICS_JOLT "Build the Jolt Physics dynamics backend (needs SAIGE_JOLT_ROOT)" OFF)
option(SAIGE_JOLT_DOUBLE "Build Jolt with double-precision world positions" OFF)
set(SAIGE_JOLT_ROOT "" CACHE PATH "Directory containing Jolt/ (see tools/physics_eval/fetch_jolt.py)")
add_library(saige_dynamics STATIC src/dynamics.cpp src/ragdoll.cpp src/buoyancy.cpp src/destruction.cpp)
add_library(saige::dynamics ALIAS saige_dynamics)
target_link_libraries(saige_dynamics PUBLIC saige_core)
if(SAIGE_PHYSICS_JOLT)
    if(NOT EXISTS "${SAIGE_JOLT_ROOT}/Jolt/Jolt.h")
        message(FATAL_ERROR "SAIGE_PHYSICS_JOLT needs SAIGE_JOLT_ROOT to hold Jolt/Jolt.h (run tools/physics_eval/fetch_jolt.py DIR --tag v5.7.0)")
    endif()
    file(GLOB_RECURSE SAIGE_JOLT_SOURCES CONFIGURE_DEPENDS "${SAIGE_JOLT_ROOT}/Jolt/*.cpp")
    list(FILTER SAIGE_JOLT_SOURCES EXCLUDE REGEX "/Jolt/(Renderer|Compute)/")
    add_library(saige_jolt STATIC ${SAIGE_JOLT_SOURCES})
    target_include_directories(saige_jolt SYSTEM PUBLIC "${SAIGE_JOLT_ROOT}")
    target_compile_features(saige_jolt PUBLIC cxx_std_17)
    # Every translation unit that includes Jolt must see the same configuration defines.
    target_compile_definitions(saige_jolt PUBLIC $<$<CONFIG:Debug>:JPH_ENABLE_ASSERTS> $<$<BOOL:${SAIGE_JOLT_DOUBLE}>:JPH_DOUBLE_PRECISION>)
    if(MSVC)
        target_compile_options(saige_jolt PRIVATE /W0 /fp:precise)
        target_compile_options(saige_jolt PUBLIC /Zc:__cplusplus)
    else()
        target_compile_options(saige_jolt PRIVATE -w -ffp-contract=off)
        target_compile_options(saige_jolt PUBLIC -ffp-contract=off)
    endif()
    target_sources(saige_dynamics PRIVATE engine/physics_jolt/jolt_dynamics.cpp)
    target_link_libraries(saige_dynamics PUBLIC saige_jolt)
    target_compile_definitions(saige_dynamics PUBLIC SAIGE_HAVE_JOLT=1)
endif()

# The audio mixer and models are held to the strictest warnings on every compiler (the rest of saige_core predates this rule and is held to the Graphics workflow's flags).
if(MSVC)
    set_source_files_properties(src/audio.cpp src/audio_model.cpp PROPERTIES COMPILE_OPTIONS "/W4;/WX")
else()
    set_source_files_properties(src/audio.cpp src/audio_model.cpp PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror;-Wconversion;-Wno-sign-conversion;-Werror=double-promotion")
endif()

# Audio device adapter (include/saige/audio.hpp: AudioOutput). The mixer itself is always built (saige_core, src/audio.cpp); the miniaudio device layer is optional. SAIGE_MINIAUDIO_ROOT is a directory
# holding miniaudio.h and stb_vorbis.c at the pinned release, fetched and hash-verified by tools/audio/fetch_miniaudio.py (docs/audio.md).
option(SAIGE_AUDIO_MINIAUDIO "Build the miniaudio device adapter and decoders (needs SAIGE_MINIAUDIO_ROOT)" OFF)
set(SAIGE_MINIAUDIO_ROOT "" CACHE PATH "Directory containing miniaudio.h (see tools/audio/fetch_miniaudio.py)")
if(SAIGE_AUDIO_MINIAUDIO)
    if(NOT EXISTS "${SAIGE_MINIAUDIO_ROOT}/miniaudio.h" OR NOT EXISTS "${SAIGE_MINIAUDIO_ROOT}/stb_vorbis.c")
        message(FATAL_ERROR "SAIGE_AUDIO_MINIAUDIO needs SAIGE_MINIAUDIO_ROOT to hold miniaudio.h and stb_vorbis.c (run tools/audio/fetch_miniaudio.py DIR)")
    endif()
    add_library(saige_audio_miniaudio STATIC engine/audio_miniaudio/miniaudio_output.cpp)
    add_executable(saige_audio_scene tools/audio/audio_scene.cpp)
    add_library(saige::audio_miniaudio ALIAS saige_audio_miniaudio)
    target_include_directories(saige_audio_miniaudio PUBLIC engine/audio_miniaudio PRIVATE "${SAIGE_MINIAUDIO_ROOT}")
    target_link_libraries(saige_audio_miniaudio PUBLIC saige_core ${CMAKE_DL_LIBS})
    target_link_libraries(saige_audio_scene PRIVATE saige_audio_miniaudio)
    if(MSVC)
        # miniaudio's own source is third-party: its warnings are not ours.
        set_source_files_properties(engine/audio_miniaudio/miniaudio_output.cpp PROPERTIES COMPILE_OPTIONS "/W2;/WX-")
    else()
        set_source_files_properties(engine/audio_miniaudio/miniaudio_output.cpp PROPERTIES COMPILE_OPTIONS "-w")
    endif()
endif()

add_executable(saige_sim_baseline tools/benchmarks/sim_baseline.cpp)
target_link_libraries(saige_sim_baseline PRIVATE saige_core)
add_executable(saige_water_workloads tools/benchmarks/water_workloads.cpp)
target_link_libraries(saige_water_workloads PRIVATE saige_core)

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
    foreach(suite IN ITEMS world runtime scene_hierarchy asset_id free_camera capsule_sweep capsule_triangle capsule_character capsule_slopes character_course static_collision oriented_box wheel_contact vehicle vehicle_surface character_surface character_drive contact_response character_crouch vehicle_handling vehicle_powertrain vehicle_drivetrain vehicle_damage water audio_model audio navigation large_world)
        add_executable(${suite}_test tests/${suite}_test.cpp)
        target_link_libraries(${suite}_test PRIVATE saige_core)
        add_test(NAME ${suite} COMMAND ${suite}_test)
    endforeach()

    if(SAIGE_AUDIO_MINIAUDIO)
        add_executable(audio_device_test tests/audio_device_test.cpp)
        target_link_libraries(audio_device_test PRIVATE saige::audio_miniaudio)
        add_test(NAME audio_device COMMAND audio_device_test)
    endif()

    add_executable(dynamics_test tests/dynamics_test.cpp)
    target_link_libraries(dynamics_test PRIVATE saige_dynamics)
    add_test(NAME dynamics COMMAND dynamics_test)
    set_tests_properties(dynamics PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(destruction_test tests/destruction_test.cpp)
    target_link_libraries(destruction_test PRIVATE saige_dynamics)
    add_test(NAME destruction COMMAND destruction_test)
    set_tests_properties(destruction PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(buoyancy_test tests/buoyancy_test.cpp)
    target_link_libraries(buoyancy_test PRIVATE saige_dynamics)
    add_test(NAME buoyancy COMMAND buoyancy_test)
    set_tests_properties(buoyancy PROPERTIES SKIP_RETURN_CODE 77)
    add_executable(ragdoll_test tests/ragdoll_test.cpp)
    target_link_libraries(ragdoll_test PRIVATE saige_dynamics)
    add_test(NAME ragdoll COMMAND ragdoll_test)
    set_tests_properties(ragdoll PROPERTIES SKIP_RETURN_CODE 77)

    target_include_directories(character_course_test PRIVATE apps)

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
        target_include_directories(win32_platform_test PRIVATE apps)
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
    add_test(NAME win32_character_course_vulkan COMMAND saige_desktop --character-course --frames 8 --validate)
    add_test(NAME win32_vehicle_course_vulkan COMMAND saige_desktop --character-course --course-vehicle --frames 8 --validate)
    set_tests_properties(win32_vehicle_course_vulkan PROPERTIES
        TIMEOUT 60 PASS_REGULAR_EXPRESSION "instances=5([^0-9]|$)")
    set_tests_properties(win32_vulkan_lifecycle win32_character_course_vulkan PROPERTIES TIMEOUT 60)
    set_tests_properties(win32_character_course_vulkan PROPERTIES PASS_REGULAR_EXPRESSION "instances=4([^0-9]|$)")
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
    find_package(Threads REQUIRED)
    add_library(saige_scene_project STATIC src/chunk_manifest.cpp src/chunk_stream.cpp src/prepared_scene.cpp)
    add_library(saige::scene_project ALIAS saige_scene_project)
    target_link_libraries(saige_scene_project PUBLIC saige::project saige::gfx_assets PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
    if(TARGET saige_desktop)
        target_link_libraries(saige_desktop PRIVATE saige::scene_project)
        target_compile_definitions(saige_desktop PRIVATE SAIGE_DESKTOP_COURSE_FILES)
        if(SAIGE_TEST_WIN32_VULKAN)
            add_test(NAME win32_character_course_file_vulkan COMMAND saige_desktop --character-course
                --course-collision "${CMAKE_CURRENT_SOURCE_DIR}/examples/character-course/course.collision.json"
                --frames 8 --validate)
            set_tests_properties(win32_character_course_file_vulkan PROPERTIES
                TIMEOUT 60 PASS_REGULAR_EXPRESSION "instances=3([^0-9]|$)")
            add_executable(streamed_course_vulkan_test tests/streamed_course_vulkan_test.cpp)
            target_include_directories(streamed_course_vulkan_test PRIVATE apps)
            target_link_libraries(streamed_course_vulkan_test PRIVATE saige::scene_project saige::render_sink saige::rhi_vulkan)
            add_test(NAME streamed_course_pixels_vulkan COMMAND streamed_course_vulkan_test
                "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured-scene/course-world.json")
            set_tests_properties(streamed_course_pixels_vulkan PROPERTIES TIMEOUT 60)
            add_test(NAME win32_character_course_stream_vulkan COMMAND saige_desktop --character-course
                --course-world "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured-scene/course-world.json"
                --frames 30 --validate)
            set_tests_properties(win32_character_course_stream_vulkan PROPERTIES
                TIMEOUT 60 PASS_REGULAR_EXPRESSION "resident_chunks=2 gpu_models=1 model_draws=[1-9][0-9]* unresolved=0 gpu_uploads=1([^0-9]|$)")
            add_test(NAME win32_ramp_course_vulkan COMMAND saige_desktop --character-course
                --course-world "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured-scene/ramp-course-world.json"
                --frames 30 --validate)
            set_tests_properties(win32_ramp_course_vulkan PROPERTIES TIMEOUT 60
                PASS_REGULAR_EXPRESSION "resident_chunks=2 gpu_models=2 model_draws=[1-9][0-9]* unresolved=0 gpu_uploads=2([^0-9]|$)")
            add_test(NAME win32_streamed_vehicle_course_vulkan COMMAND saige_desktop --character-course --course-vehicle
                --course-world "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured-scene/ramp-course-world.json"
                --frames 30 --validate)
            set_tests_properties(win32_streamed_vehicle_course_vulkan PROPERTIES TIMEOUT 60
                PASS_REGULAR_EXPRESSION "resident_chunks=2 gpu_models=2 model_draws=[1-9][0-9]* unresolved=0 gpu_uploads=2([^0-9]|$)")
            add_test(NAME win32_terrain_vehicle_vulkan COMMAND saige_desktop --character-course --course-vehicle --terrain-vehicle
                --course-world "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured-scene/ramp-course-world.json"
                --frames 30 --require-rendered --validate)
            set_tests_properties(win32_terrain_vehicle_vulkan PROPERTIES TIMEOUT 60
                PASS_REGULAR_EXPRESSION "resident_chunks=2 gpu_models=2 model_draws=[1-9][0-9]* unresolved=0")
            set(course_save_fixture "${CMAKE_CURRENT_BINARY_DIR}/streamed-vehicle-checkpoint.json")
            file(WRITE "${course_save_fixture}" [=[
{"checkpoint_version":2,"cell":[2147483646,-2147483648],"feet":[3,4,0],"vehicle":{"cell":[2147483646,-2147483648],"position":[3,1,0],"heading":0}}
]=])
            add_test(NAME win32_streamed_vehicle_save_vulkan COMMAND saige_desktop --character-course --course-vehicle
                --course-world "${CMAKE_CURRENT_SOURCE_DIR}/examples/textured-scene/ramp-course-world.json"
                --course-checkpoint "${course_save_fixture}" --frames 30 --require-rendered --validate)
            set_tests_properties(win32_streamed_vehicle_save_vulkan PROPERTIES TIMEOUT 60
                PASS_REGULAR_EXPRESSION "resident_chunks=2 gpu_models=2 model_draws=[1-9][0-9]* unresolved=0 .*world_cell=2147483646,-2147483648")
        endif()
    endif()
    add_executable(saige_scene_asset_tool apps/scene_asset_tool.cpp)
    target_link_libraries(saige_scene_asset_tool PRIVATE saige::scene_project)
    if(BUILD_TESTING)
        add_executable(chunk_stream_test tests/chunk_stream_test.cpp)
        target_link_libraries(chunk_stream_test PRIVATE saige::scene_project)
        target_include_directories(chunk_stream_test PRIVATE apps)
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

add_subdirectory(showcase)
add_subdirectory(tools/production/inspect)
add_subdirectory(tools/animation/viewer)
