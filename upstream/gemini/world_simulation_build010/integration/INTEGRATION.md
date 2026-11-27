# SAIGE Integration Instructions (Build 006)

- **Target Branch:** `gemini/world-simulation` (branched from `claude/production-infrastructure` at `d15bfd3`)
- **C++ Standard:** C++20 (`-Wall -Wextra -Wpedantic -Werror -Wconversion`)
- **Native Coordinate Bridge:** Define `SAIGE_HAS_NATIVE_LARGE_WORLD=1` and use `include/saige/saige_world_adapter.hpp` (`from_native_world_position`, `to_native_world_position`, `validate_native_roundtrip`).
- **Authoritative Physics Feedback:** Set `TrafficVehicle::use_external_physics = true`. `TrafficSimulation` emits `VehicleControlInput` (including `BrakeReason` and `LaneChangeCommand` + `target_lane_id`); `src/vehicle.cpp` owns actual lane transitions, `s_m`, and `speed_mps`, syncing back via `sync_external_vehicle_state()`.
