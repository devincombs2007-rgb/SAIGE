# saige::render_sink: the graphics adapter implementing Core's saige::IFrameSink (docs/core-rendering-contract.md).
# Built from the root CMakeLists (needs saige::core). Backend-agnostic: the application chooses the rhi::Device.
cmake_minimum_required(VERSION 3.21)

if(NOT TARGET saige::core)
    message(FATAL_ERROR "engine/render_sink must be built from the SAIGE root CMakeLists (needs saige::core)")
endif()

add_library(saige_render_sink STATIC src/renderer_frame_sink.cpp)
add_library(saige::render_sink ALIAS saige_render_sink)
target_include_directories(saige_render_sink PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(saige_render_sink PUBLIC saige::core saige::render saige::gfx_scene)
saige_gfx_configure_target(saige_render_sink)

if(BUILD_TESTING)
    saige_gfx_add_test(saige_render_sink_tests
        SOURCES tests/test_renderer_frame_sink.cpp
        LINK saige::render_sink saige::rhi_null)
    if(TARGET saige::rhi_vulkan)
        add_executable(saige_render_sink_vulkan_tests tests/test_vulkan_frame_sink.cpp)
        target_include_directories(saige_render_sink_vulkan_tests PRIVATE
            ${SAIGE_GFX_SOURCE_ROOT}/engine/gfx_test_support/include)
        target_link_libraries(saige_render_sink_vulkan_tests PRIVATE saige::render_sink saige::rhi_vulkan)
        saige_gfx_configure_target(saige_render_sink_vulkan_tests)
        add_test(NAME saige_render_sink_vulkan_tests COMMAND saige_render_sink_vulkan_tests)
        set_tests_properties(saige_render_sink_vulkan_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
        # The 0.3 renderer end to end: staged glTF with five textures, metallic-roughness, normal mapping and HDR together.
        add_executable(saige_render_sink_combined_vulkan_tests tests/test_combined_pbr.cpp)
        target_include_directories(saige_render_sink_combined_vulkan_tests PRIVATE
            ${SAIGE_GFX_SOURCE_ROOT}/engine/gfx_test_support/include
            ${SAIGE_GFX_SOURCE_ROOT}/engine/rhi_vulkan/tests)
        target_link_libraries(saige_render_sink_combined_vulkan_tests PRIVATE saige::render_sink saige::rhi_vulkan)
        saige_gfx_configure_target(saige_render_sink_combined_vulkan_tests)
        add_test(NAME saige_render_sink_combined_vulkan_tests COMMAND saige_render_sink_combined_vulkan_tests)
        set_tests_properties(saige_render_sink_combined_vulkan_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endif()
endif()

# End-to-end scene tests: Core's saved textured project (saige::project is defined after this directory is processed, so the
# registration is deferred to the end of the root directory) through GPU staging and the sink on Vulkan. Exit 77 = no Vulkan
# device (skipped, never passed).
set_property(GLOBAL PROPERTY SAIGE_RENDER_SINK_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
function(saige_add_scene_sink_tests)
    get_property(render_sink_dir GLOBAL PROPERTY SAIGE_RENDER_SINK_DIR)
    if(BUILD_TESTING AND TARGET saige::rhi_vulkan AND TARGET saige::project)
        add_executable(saige_render_sink_scene_vulkan_tests ${render_sink_dir}/tests/test_scene_sink.cpp)
        target_include_directories(saige_render_sink_scene_vulkan_tests PRIVATE ${SAIGE_GFX_SOURCE_ROOT}/engine/gfx_test_support/include)
        target_link_libraries(saige_render_sink_scene_vulkan_tests PRIVATE saige::render_sink saige::rhi_vulkan saige::project)
        target_compile_definitions(saige_render_sink_scene_vulkan_tests PRIVATE
            SAIGE_TEXTURED_SCENE_DIR="${render_sink_dir}/../../examples/textured-scene")
        saige_gfx_configure_target(saige_render_sink_scene_vulkan_tests)
        add_test(NAME saige_render_sink_scene_vulkan_tests COMMAND saige_render_sink_scene_vulkan_tests)
        set_tests_properties(saige_render_sink_scene_vulkan_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endif()
    # The desktop sample's `--scene` mode: Core's PreparedScene through GPU staging into the sink on a real window.
    if(TARGET saige_desktop AND TARGET saige::rhi_vulkan AND TARGET saige::scene_project)
        target_link_libraries(saige_desktop PRIVATE saige::scene_project)
        target_compile_definitions(saige_desktop PRIVATE SAIGE_DESKTOP_SCENE)
        if(SAIGE_TEST_WIN32_VULKAN)
            add_test(NAME saige_desktop_scene_vulkan COMMAND saige_desktop --scene
                "${render_sink_dir}/../../examples/textured-scene/saige.scene.json" --frames 30 --validate)
            set_tests_properties(saige_desktop_scene_vulkan PROPERTIES TIMEOUT 60)
        endif()
    endif()
    # The native app's flow (PreparedScene -> stage_prepared_scene -> activate_scene -> run) with a fake desktop.
    if(BUILD_TESTING AND TARGET saige::rhi_vulkan AND TARGET saige::scene_project)
        add_executable(saige_render_sink_prepared_scene_vulkan_tests ${render_sink_dir}/tests/test_prepared_scene.cpp)
        target_include_directories(saige_render_sink_prepared_scene_vulkan_tests PRIVATE ${SAIGE_GFX_SOURCE_ROOT}/engine/gfx_test_support/include)
        target_link_libraries(saige_render_sink_prepared_scene_vulkan_tests PRIVATE saige::render_sink saige::rhi_vulkan saige::scene_project)
        target_compile_definitions(saige_render_sink_prepared_scene_vulkan_tests PRIVATE
            SAIGE_TEXTURED_SCENE_DIR="${render_sink_dir}/../../examples/textured-scene")
        saige_gfx_configure_target(saige_render_sink_prepared_scene_vulkan_tests)
        add_test(NAME saige_render_sink_prepared_scene_vulkan_tests COMMAND saige_render_sink_prepared_scene_vulkan_tests)
        set_tests_properties(saige_render_sink_prepared_scene_vulkan_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
    endif()
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL saige_add_scene_sink_tests)

# Desktop wiring (integration with Core's apps/desktop.cpp): when the Win32 desktop sample and the Vulkan backend are
# both built, the sample renders through RendererFrameSink. Without a Vulkan device it falls back to its probe.
if(TARGET saige_desktop AND TARGET saige::rhi_vulkan)
    target_link_libraries(saige_desktop PRIVATE saige::render_sink saige::rhi_vulkan)
    target_compile_definitions(saige_desktop PRIVATE SAIGE_DESKTOP_VULKAN)
endif()

# Windows: the sink on a real HWND (Core's Win32Platform). Complements Core's tests/win32_vulkan_test.cpp (surface
# lifecycle, keyboard, shutdown order) by reading the *window* back with GDI and asserting the rendered cube is on
# screen after the first present, a resize and a restore. Exit 77 = no Vulkan device (skipped, never passed).
if(WIN32 AND BUILD_TESTING AND TARGET saige::rhi_vulkan AND TARGET saige::desktop)
    add_executable(saige_win32_vulkan_tests tests/test_win32_vulkan.cpp)
    target_include_directories(saige_win32_vulkan_tests PRIVATE ${SAIGE_GFX_SOURCE_ROOT}/engine/gfx_test_support/include)
    target_link_libraries(saige_win32_vulkan_tests PRIVATE saige::render_sink saige::rhi_vulkan saige::desktop)
    target_compile_definitions(saige_win32_vulkan_tests PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    saige_gfx_configure_target(saige_win32_vulkan_tests)
    add_test(NAME saige_win32_vulkan_tests COMMAND saige_win32_vulkan_tests)
    set_tests_properties(saige_win32_vulkan_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 180)
endif()
