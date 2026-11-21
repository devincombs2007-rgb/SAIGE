cmake_minimum_required(VERSION 3.20)
project(SAIGE VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

add_library(saige_core STATIC
    src/runtime.cpp
    src/world.cpp
)
target_include_directories(saige_core PUBLIC include)
target_compile_features(saige_core PUBLIC cxx_std_20)

add_executable(saige_headless apps/headless.cpp)
target_link_libraries(saige_headless PRIVATE saige_core)

include(CTest)
if(BUILD_TESTING)
    foreach(suite IN ITEMS world runtime)
        add_executable(${suite}_test tests/${suite}_test.cpp)
        target_link_libraries(${suite}_test PRIVATE saige_core)
        add_test(NAME ${suite} COMMAND ${suite}_test)
    endforeach()

    add_test(NAME headless_boot COMMAND saige_headless --frames 3)
    set_tests_properties(headless_boot PROPERTIES
        PASS_REGULAR_EXPRESSION "frames=3([^0-9]|$)"
        TIMEOUT 10
    )
endif()
