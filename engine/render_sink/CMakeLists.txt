# saige::render_sink: the graphics adapter implementing Core's saige::IFrameSink (docs/core-rendering-contract.md).
# Built from the root CMakeLists (needs saige::core). Backend-agnostic: the application chooses the rhi::Device.
cmake_minimum_required(VERSION 3.21)

if(NOT TARGET saige::core)
    message(FATAL_ERROR "engine/render_sink must be built from the SAIGE root CMakeLists (needs saige::core)")
endif()

add_library(saige_render_sink STATIC src/renderer_frame_sink.cpp)
add_library(saige::render_sink ALIAS saige_render_sink)
target_include_directories(saige_render_sink PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(saige_render_sink PUBLIC saige::core saige::render)
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
    endif()
endif()

# Desktop wiring (integration with Core's apps/desktop.cpp): when the Win32 desktop sample and the Vulkan backend are
# both built, the sample renders through RendererFrameSink. Without a Vulkan device it falls back to its probe.
if(TARGET saige_desktop AND TARGET saige::rhi_vulkan)
    target_link_libraries(saige_desktop PRIVATE saige::render_sink saige::rhi_vulkan)
    target_compile_definitions(saige_desktop PRIVATE SAIGE_DESKTOP_VULKAN)
endif()
