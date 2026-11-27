# SAIGE Super Resolution (SSR Build 002) CMake Integration Snippet
add_library(saige_ssr STATIC
  source/src/ssr.cpp
  gpu/ssr_vulkan_harness.cpp
)
target_include_directories(saige_ssr PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/source/include
  ${CMAKE_CURRENT_SOURCE_DIR}/gpu
)
target_compile_features(saige_ssr PUBLIC cxx_std_20)
target_compile_options(saige_ssr PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror -Wconversion>
)

find_package(Vulkan QUIET)
if(Vulkan_FOUND)
  target_compile_definitions(saige_ssr PUBLIC SAIGE_SSR_ENABLE_VULKAN=1)
  target_link_libraries(saige_ssr PUBLIC Vulkan::Vulkan)
endif()

# Optional SPIR-V compilation when glslangValidator is present
find_program(GLSLANG_VALIDATOR glslangValidator)
if(GLSLANG_VALIDATOR)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ssr_reconstruct.spv
    COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.4
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ssr_reconstruct.comp
            -o ${CMAKE_CURRENT_BINARY_DIR}/ssr_reconstruct.spv
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ssr_reconstruct.comp
  )
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ssr_sharpen.spv
    COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.4
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ssr_sharpen.comp
            -o ${CMAKE_CURRENT_BINARY_DIR}/ssr_sharpen.spv
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/ssr_sharpen.comp
  )
  add_custom_target(saige_ssr_shaders ALL
    DEPENDS
      ${CMAKE_CURRENT_BINARY_DIR}/ssr_reconstruct.spv
      ${CMAKE_CURRENT_BINARY_DIR}/ssr_sharpen.spv
  )
endif()
