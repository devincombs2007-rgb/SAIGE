# saige_anim_viewer: the humanoid animation module on the real renderer (UAL character on a small course, rendered offscreen). Needs the Vulkan backend and the scene staging.
if(TARGET saige::animation AND TARGET saige::core AND TARGET saige::rhi_vulkan AND TARGET saige::gfx_scene AND TARGET saige::gfx_assets AND TARGET saige::render)
    add_executable(saige_anim_viewer anim_viewer.cpp)
    target_link_libraries(saige_anim_viewer PRIVATE saige::animation saige::core saige::rhi_vulkan saige::gfx_scene saige::gfx_assets saige::render)
    saige_gfx_configure_target(saige_anim_viewer)
    if(BUILD_TESTING)
        # Renders a few frames of the scripted run. Exit 77 (skipped) when the humanoid assets are not fetched (SAIGE_ANIM_ASSETS) or there is no Vulkan 1.3 device.
        add_test(NAME animation_viewer_smoke COMMAND saige_anim_viewer --out ${CMAKE_CURRENT_BINARY_DIR}/smoke --width 320 --height 180 --every 40 --seconds 6 --self-check)
        set_tests_properties(animation_viewer_smoke PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
    endif()
endif()
