# Additive World Simulation & AI sources for saige_core (C++20)
target_sources(saige_core PRIVATE
  src/npc_simulation.cpp
  src/crowd_avoidance.cpp
  src/traffic_simulation.cpp
  src/multi_floor_nav.cpp
  src/persistent_world.cpp
  src/npc_psychology.cpp
  src/social_simulation.cpp
  src/local_economy.cpp
)

if(BUILD_TESTING)
  foreach(sim_test npc_simulation_test crowd_avoidance_test traffic_simulation_test multi_floor_nav_test persistent_world_test npc_psychology_test social_simulation_test local_economy_test)
    add_executable(${sim_test} tests/${sim_test}.cpp)
    target_link_libraries(${sim_test} PRIVATE saige_core)
    target_compile_features(${sim_test} PRIVATE cxx_std_20)
    add_test(NAME ${sim_test} COMMAND ${sim_test})
  endforeach()
endif()
