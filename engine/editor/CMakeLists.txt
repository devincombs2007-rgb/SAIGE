# World editor core (from Gemini World Editor Build 012; upstream copy in upstream/gemini/world_editor_build012, integration record in
# docs/world-editor/build012-integration.md). Scene, commands, project persistence, workspace, asset library, semantic placement and material
# authoring; no renderer or window-system dependency.
add_library(saige_editor STATIC
    src/editor_scene.cpp
    src/editor_commands.cpp
    src/editor_project.cpp
    src/editor_workspace.cpp
    src/editor_assets.cpp
    src/editor_semantic.cpp
    src/editor_materials.cpp
)
add_library(saige::editor ALIAS saige_editor)
target_include_directories(saige_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(saige_editor PUBLIC cxx_std_20)
target_compile_options(saige_editor PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow>)

if(BUILD_TESTING)
    foreach(t IN ITEMS editor_core_test editor_build012_test editor_regression_test)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE saige_editor)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()
endif()
