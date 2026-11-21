# SaigeGraphics.cmake — shared build settings for SAIGE graphics modules (rhi, render, gfx_assets, tools).
#
# Owner: Rendering. Follows Core's conventions (issue #2: D-02 CMake 3.21, D-05 compiler defaults for exceptions/RTTI).

include_guard(GLOBAL)

option(SAIGE_GFX_WARNINGS_AS_ERRORS "Treat warnings as errors in graphics modules" OFF)
option(SAIGE_GFX_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer (GCC/Clang)" OFF)

# saige_gfx_configure_target(<target>)
#   Applies C++20, warning level and sanitizers. Exceptions and RTTI keep compiler defaults (Core D-05).
function(saige_gfx_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)
    set_target_properties(${target} PROPERTIES CXX_EXTENSIONS OFF)

    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8 /EHsc)
        # Standard C functions such as std::getenv are not deprecated; keep MSVC's C4996 from failing /WX builds.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(SAIGE_GFX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
            -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough)
        if(SAIGE_GFX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
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
