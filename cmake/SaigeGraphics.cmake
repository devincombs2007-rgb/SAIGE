# SaigeGraphics.cmake — shared build settings for SAIGE graphics modules (rhi, render, gfx_assets, tools).
#
# Owner: Rendering. Temporary until Core publishes engine-wide build conventions (issue #2, D-02/D-05).
# When Core's root CMake exists, these helpers should defer to (or be replaced by) Core's equivalents.

include_guard(GLOBAL)

option(SAIGE_GFX_WARNINGS_AS_ERRORS "Treat warnings as errors in graphics modules" OFF)
option(SAIGE_GFX_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

# saige_gfx_configure_target(<target> [RUNTIME])
#   Applies C++20, warning level and sanitizers. With RUNTIME, also disables exceptions and RTTI
#   (runtime engine code policy, proposed in D-05). Test and tool targets omit RUNTIME.
function(saige_gfx_configure_target target)
    cmake_parse_arguments(ARG "RUNTIME" "" "" ${ARGN})

    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
        if(SAIGE_GFX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        if(ARG_RUNTIME)
            target_compile_options(${target} PRIVATE /GR-)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
            -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough)
        if(SAIGE_GFX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(ARG_RUNTIME)
            target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
        endif()
        if(SAIGE_GFX_SANITIZERS)
            target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
            target_link_options(${target} PUBLIC -fsanitize=address,undefined)
        endif()
    endif()
endfunction()

set(SAIGE_GFX_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." CACHE INTERNAL "SAIGE repository root (graphics modules)")

# saige_gfx_add_test(<name> SOURCES <files...> LINK <targets...>)
#   Creates a test executable using the shared graphics test harness (temporary until D-17) and registers it with CTest.
function(saige_gfx_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LINK" ${ARGN})

    if(NOT TARGET saige_gfx_test_main)
        add_library(saige_gfx_test_main STATIC ${SAIGE_GFX_SOURCE_ROOT}/engine/gfx_test_support/src/test_main.cpp)
        target_include_directories(saige_gfx_test_main PUBLIC ${SAIGE_GFX_SOURCE_ROOT}/engine/gfx_test_support/include)
        saige_gfx_configure_target(saige_gfx_test_main)
    endif()

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE saige_gfx_test_main ${ARG_LINK})
    saige_gfx_configure_target(${name})
    add_test(NAME ${name} COMMAND ${name})
endfunction()
