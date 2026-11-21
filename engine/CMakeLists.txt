# SAIGE graphics modules (owner: Rendering, see AGENTS.md). Included from the root CMakeLists via
# add_subdirectory(engine) when SAIGE_BUILD_GRAPHICS is ON. Each module also builds standalone.

add_subdirectory(gfx_test_support)
add_subdirectory(rhi)
add_subdirectory(rhi_null)
add_subdirectory(render)

option(SAIGE_ENABLE_VULKAN "Build the Vulkan 1.3 RHI backend (needs Vulkan headers; see engine/rhi_vulkan)" OFF)
if(SAIGE_ENABLE_VULKAN)
    add_subdirectory(rhi_vulkan)
endif()

# Adapter to Core's IFrameSink (needs saige::core, so only in the root build).
if(TARGET saige::core)
    add_subdirectory(render_sink)
endif()
