add_library(saige_editor STATIC
  source/src/editor_scene.cpp
  source/src/editor_commands.cpp
  source/src/editor_project.cpp
  source/src/editor_workspace.cpp
  source/src/editor_assets.cpp
  source/src/editor_semantic.cpp
  source/src/editor_materials.cpp
)
target_include_directories(saige_editor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source/include)
target_compile_features(saige_editor PUBLIC cxx_std_20)
target_compile_options(saige_editor PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -Wconversion>
)
