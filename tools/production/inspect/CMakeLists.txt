# The runtime check of the asset pipeline: the engine's own glTF importer applied to a candidate file.
if(TARGET saige::gfx_assets AND TARGET saige::render)
    add_executable(saige_asset_inspect main.cpp)
    target_link_libraries(saige_asset_inspect PRIVATE saige::gfx_assets saige::render)
    saige_gfx_configure_target(saige_asset_inspect)
endif()
