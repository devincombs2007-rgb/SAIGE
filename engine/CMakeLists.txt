# SAIGE graphics modules (owner: Rendering, see AGENTS.md). Included from the root CMakeLists via
# add_subdirectory(engine) when SAIGE_BUILD_GRAPHICS is ON. Each module also builds standalone.

add_subdirectory(rhi)
add_subdirectory(render)
