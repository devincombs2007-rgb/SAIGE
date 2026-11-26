# Independent validation of Gemini's SSR Build 002.1 (docs/validation/2026-11-26-ssr-build-002.1, validation/ssr/README.md).
# Isolated: nothing here is linked into the production renderer, and nothing here is built unless a Vulkan loader, the tools below and the
# pinned Vulkan headers are all present (the hosted Linux lavapipe lane has them; the ASan and Windows lanes skip it silently).
# The delivered sources stay verbatim in upstream/ (manifest-checked); only harness/ is a modified copy, every change marked "VALIDATION CHANGE".
if(NOT UNIX OR NOT BUILD_TESTING OR NOT TARGET saige_vulkan_headers)
    return()
endif()
find_library(SAIGE_SSR_VULKAN_LOADER vulkan)
find_program(SAIGE_SSR_GLSLANG glslangValidator)
find_program(SAIGE_SSR_SPIRV_VAL spirv-val)
find_program(SAIGE_SSR_SPIRV_DIS spirv-dis)
find_package(Python3 COMPONENTS Interpreter)
if(NOT SAIGE_SSR_VULKAN_LOADER OR NOT SAIGE_SSR_GLSLANG OR NOT SAIGE_SSR_SPIRV_VAL OR NOT SAIGE_SSR_SPIRV_DIS OR NOT Python3_FOUND)
    message(STATUS "SSR validation: skipped (needs libvulkan, glslangValidator, spirv-val, spirv-dis and Python 3)")
    return()
endif()

set(SSR_MUTANTS alpha_raised confidence_constant cut_ignored decompress_floor depth_history_zero depth_test_flipped exposure_ignored jitter_sign
    mv_sign no_disocclusion no_history_clamp sharpen_north_south sharpen_off transparency_ignored)
string(REPLACE ";" "," SSR_MUTANTS_CSV "${SSR_MUTANTS}")
set(SSR_SHADER_DIR ${CMAKE_CURRENT_BINARY_DIR}/shaders)

add_custom_command(
    OUTPUT ${SSR_SHADER_DIR}/shaders.stamp
    COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_ssr_shaders.py --glslang ${SAIGE_SSR_GLSLANG} --spirv-val ${SAIGE_SSR_SPIRV_VAL}
            --src ${CMAKE_CURRENT_SOURCE_DIR}/upstream/shaders --out ${SSR_SHADER_DIR} --expect-mutants ${SSR_MUTANTS_CSV}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/upstream/shaders/ssr_reconstruct.comp ${CMAKE_CURRENT_SOURCE_DIR}/upstream/shaders/ssr_sharpen.comp
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/build_ssr_shaders.py
    COMMENT "SSR validation: compiling and validating the SPIR-V (fp32, fp16, sharpen, mutants)")
add_custom_target(saige_ssr_validation_shaders ALL DEPENDS ${SSR_SHADER_DIR}/shaders.stamp)

function(saige_ssr_executable name)  # name, sources..., defines via SSR_DEFS
    add_executable(${name} ${ARGN})
    target_compile_features(${name} PRIVATE cxx_std_20)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/upstream/source/include ${CMAKE_CURRENT_SOURCE_DIR}/harness)
    target_compile_definitions(${name} PRIVATE SAIGE_SSR_ENABLE_VULKAN=1 SSR_VALIDATION_SHADER_DIR="${SSR_SHADER_DIR}" ${SSR_DEFS})
    target_link_libraries(${name} PRIVATE saige_vulkan_headers ${SAIGE_SSR_VULKAN_LOADER})
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        # -O2 whatever the build type: the CPU reference is a per-pixel scalar model and a Debug build of it would dominate the lane's time.
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow -O2)
    endif()
    add_dependencies(${name} saige_ssr_validation_shaders)
endfunction()

set(SSR_CORE_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/upstream/source/src/ssr.cpp ${CMAKE_CURRENT_SOURCE_DIR}/harness/ssr_vulkan_harness.cpp)

# The validation suite (exit 77 = no Vulkan device or validation layer: reported as skipped, never as passed).
set(SSR_DEFS "")
saige_ssr_executable(ssr_validation_test ${SSR_CORE_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ssr_validation_test.cpp)
add_test(NAME ssr_validation COMMAND ssr_validation_test)
set_tests_properties(ssr_validation PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

# Gemini's own suite, unmodified except that its include of the harness header is redirected to the corrected harness (the delivered file includes
# "../gpu/ssr_vulkan_harness.hpp", which with this layout would silently pick the delivered, differently sized VulkanParityReport: an ODR trap).
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/upstream/tests/ssr_test.cpp SSR_UPSTREAM_TEST)
string(REPLACE "\"../gpu/ssr_vulkan_harness.hpp\"" "\"ssr_vulkan_harness.hpp\"" SSR_UPSTREAM_TEST "${SSR_UPSTREAM_TEST}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ssr_test_upstream.cpp "${SSR_UPSTREAM_TEST}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/upstream/tests/ssr_test.cpp)
saige_ssr_executable(ssr_upstream_suite ${SSR_CORE_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/ssr_test_upstream.cpp)
add_test(NAME ssr_upstream_suite COMMAND ssr_upstream_suite WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(ssr_upstream_suite PROPERTIES TIMEOUT 900)

# SPIR-V interface against the host structures (descriptor types and bindings, push-constant offsets).
add_test(NAME ssr_spirv_interface COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/tools/check_spirv_interface.py ${SAIGE_SSR_SPIRV_DIS} ${SSR_SHADER_DIR})

# Shader mutants: one textual change each in Gemini's shaders; the suite's mutant mode must DETECT every one (exit 1 = detected, so WILL_FAIL).
foreach(m IN LISTS SSR_MUTANTS)
    if(m MATCHES "^sharpen_")
        set(_recon ${SSR_SHADER_DIR}/ssr_reconstruct.spv)
        set(_sharp ${SSR_SHADER_DIR}/mutants/${m}.spv)
    else()
        set(_recon ${SSR_SHADER_DIR}/mutants/${m}.spv)
        set(_sharp ${SSR_SHADER_DIR}/ssr_sharpen.spv)
    endif()
    add_test(NAME ssr_mutant_${m} COMMAND ssr_validation_test --mutant ${_recon} ${_sharp})
    set_tests_properties(ssr_mutant_${m} PROPERTIES WILL_FAIL TRUE SKIP_RETURN_CODE 77 TIMEOUT 300)
endforeach()

# Harness mutants: the delivered behaviours put back, one at a time. The suite must fail on each.
foreach(hm EMPTY_REFERENCE_PASSES LAYOUT_COLOR_ONLY VALIDATION_ALWAYS_CLEAN)
    set(SSR_DEFS SSR_VALIDATION_MUTANT_${hm}=1)
    string(TOLOWER ${hm} _hm_l)
    saige_ssr_executable(ssr_harness_mutant_${_hm_l} ${SSR_CORE_SOURCES} ${CMAKE_CURRENT_SOURCE_DIR}/tests/ssr_validation_test.cpp)
    add_test(NAME ssr_harness_mutant_${_hm_l} COMMAND ssr_harness_mutant_${_hm_l})
    set_tests_properties(ssr_harness_mutant_${_hm_l} PROPERTIES WILL_FAIL TRUE SKIP_RETURN_CODE 77 TIMEOUT 600)
endforeach()

# Independent visual and measurement evidence (not a test): Gemini's scenes, TAA-U baseline and metrics, plus the GPU path. Generated from the
# delivered test source (main renamed) so nothing is re-implemented.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/tools/ssr_visual_dump_main.inc SSR_VISUAL_MAIN)
string(REPLACE "int main(int argc, char** argv) {" "int upstream_main(int argc, char** argv) {" SSR_VISUAL_BASE "${SSR_UPSTREAM_TEST}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/ssr_visual_dump.cpp "${SSR_VISUAL_BASE}\n${SSR_VISUAL_MAIN}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/ssr_visual_dump_main.inc)
set(SSR_DEFS "")
saige_ssr_executable(ssr_visual_dump ${SSR_CORE_SOURCES} ${CMAKE_CURRENT_BINARY_DIR}/ssr_visual_dump.cpp)
