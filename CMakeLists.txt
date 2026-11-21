cmake_minimum_required(VERSION 3.21)
project(SAIGE VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

add_library(saige_core STATIC
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
    foreach(suite IN ITEMS world runtime scene_hierarchy)
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
