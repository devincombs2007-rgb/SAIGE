# SAIGE shaders (owner: Rendering; docs/rendering/shader-pipeline.md).
#
# HLSL sources live in shaders/<group>/. Compiled SPIR-V is checked in as C++ headers under
# shaders/generated/saige/shaders/, so engine builds never need a shader compiler. After editing HLSL, run:
#   cmake --build <build-dir> --target saige_regenerate_shaders
# CTest checks that every generated header matches its sources (no compiler needed), and, when a compiler is
# available, that every shader still compiles and passes spirv-val.

set(SAIGE_SHADER_SOURCES
    scene/cube.hlsl
    scene/ground.hlsl
    scene/model.hlsl
    scene/model_shadowed.hlsl
    scene/model_ibl.hlsl
    scene/model_ibl_shadowed.hlsl
    scene/model_skinned.hlsl
    scene/shadow_depth.hlsl
    scene/sky.hlsl
    scene/shadow_depth_skinned.hlsl
    test/blend_color.hlsl
    test/depth_only.hlsl
    test/shadow_probe.hlsl
    scene/tonemap.hlsl
    test/textured_fullscreen.hlsl
)

add_library(saige_shaders INTERFACE)
add_library(saige::shaders ALIAS saige_shaders)
target_include_directories(saige_shaders INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/generated)

find_program(SAIGE_DXC_EXECUTABLE dxc)
find_program(SAIGE_GLSLANG_EXECUTABLE glslangValidator)
find_program(SAIGE_SPIRV_VAL_EXECUTABLE spirv-val)

set(SAIGE_SHADER_COMPILER "auto" CACHE STRING "Shader compiler for regeneration: auto, dxc or glslang")
set(_compiler "")
set(_compiler_path "")
if((SAIGE_SHADER_COMPILER STREQUAL "auto" OR SAIGE_SHADER_COMPILER STREQUAL "glslang") AND SAIGE_GLSLANG_EXECUTABLE)
    # 0.1 uses glslang by default so generated headers are reproducible on the Linux CI image.
    set(_compiler glslang)
    set(_compiler_path ${SAIGE_GLSLANG_EXECUTABLE})
elseif((SAIGE_SHADER_COMPILER STREQUAL "auto" OR SAIGE_SHADER_COMPILER STREQUAL "dxc") AND SAIGE_DXC_EXECUTABLE)
    set(_compiler dxc)
    set(_compiler_path ${SAIGE_DXC_EXECUTABLE})
endif()

set(_regen_commands)
foreach(src IN LISTS SAIGE_SHADER_SOURCES)
    get_filename_component(_name ${src} NAME_WE)
    get_filename_component(_group ${src} DIRECTORY)
    set(_header ${CMAKE_CURRENT_SOURCE_DIR}/generated/saige/shaders/${_group}_${_name}.spv.hpp)
    set(_source ${CMAKE_CURRENT_SOURCE_DIR}/${src})
    if(_compiler)
        list(APPEND _regen_commands
            COMMAND saige-shaderc compile ${_source}
                    --compiler ${_compiler} --compiler-path ${_compiler_path}
                    --work-dir ${CMAKE_CURRENT_BINARY_DIR}/regen
                    --embed ${_header} --label shaders/${src})
    endif()
    if(BUILD_TESTING)
        add_test(NAME saige_shader_up_to_date_${_group}_${_name}
                 COMMAND saige-shaderc verify ${_source} --embed ${_header})
        if(_compiler)
            add_test(NAME saige_shader_compiles_${_group}_${_name}
                     COMMAND saige-shaderc compile ${_source} --compiler ${_compiler} --compiler-path ${_compiler_path}
                             --work-dir ${CMAKE_CURRENT_BINARY_DIR}/test_${_group}_${_name})
            set_tests_properties(saige_shader_compiles_${_group}_${_name} PROPERTIES FIXTURES_SETUP spv_${_name})
            if(SAIGE_SPIRV_VAL_EXECUTABLE)
                add_test(NAME saige_shader_spirv_val_${_group}_${_name}
                         COMMAND ${CMAKE_COMMAND} -DSPIRV_VAL=${SAIGE_SPIRV_VAL_EXECUTABLE}
                                 -DDIR=${CMAKE_CURRENT_BINARY_DIR}/test_${_group}_${_name}
                                 -P ${CMAKE_CURRENT_SOURCE_DIR}/validate_spirv.cmake)
                set_tests_properties(saige_shader_spirv_val_${_group}_${_name} PROPERTIES FIXTURES_REQUIRED spv_${_name})
            endif()
        endif()
    endif()
endforeach()

if(_compiler)
    add_custom_target(saige_regenerate_shaders ${_regen_commands}
        COMMENT "Regenerating embedded SPIR-V headers with ${_compiler}"
        VERBATIM)
else()
    add_custom_target(saige_regenerate_shaders
        COMMAND ${CMAKE_COMMAND} -E echo "No shader compiler found (install the Vulkan SDK for dxc, or glslang)."
        COMMAND ${CMAKE_COMMAND} -E false)
endif()
