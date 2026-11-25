# The runtime check of the asset pipeline: the engine's own glTF importer applied to a candidate file.
if(TARGET saige::gfx_assets AND TARGET saige::render)
    add_executable(saige_asset_inspect main.cpp)
    target_link_libraries(saige_asset_inspect PRIVATE saige::gfx_assets saige::render)
    saige_gfx_configure_target(saige_asset_inspect)
endif()

# Offline tests of the asset catalog, extractors and prep helpers (Python). Exit code 77: the tools venv is not at its default place ($SAIGE_TOOLS_HOME or ~/.saige-tools; install it with tools/production/setup.sh), reported as skipped.
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        add_test(NAME production_tools_python COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tests/run_tests.py)
        set_tests_properties(production_tools_python PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
        # The toolchain audit's own tests (standard library only; the audit itself is run by hand or in the scheduled qualification, see tools/audit/README.md).
        # The animation asset restoration (download, archive and file hashes, retries): offline, standard library only.
        add_test(NAME animation_assets_fetch_python COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/../../animation/tests/test_fetch.py)
        set_tests_properties(animation_assets_fetch_python PROPERTIES TIMEOUT 60)
        add_test(NAME audit_tools_python COMMAND ${Python3_EXECUTABLE} -m unittest discover -s ${CMAKE_CURRENT_SOURCE_DIR}/../../audit)
        set_tests_properties(audit_tools_python PROPERTIES TIMEOUT 120)
    endif()
endif()
