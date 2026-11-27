# Changelog

## Build 010 (2026-11-25)
- **Defect 1 Fixed (`persistent_world.cpp`):** Implemented genuine Build 008 `WLD1` v1 -> `WLD2` v2 migration in `deserialize()`; added regression test.
- **Defect 2 Fixed (`persistent_world.hpp/.cpp`):** Replaced raw `find_npc_mut()` with safe `set_npc_position()` & `set_npc_balance()` that maintain `spatial_index_dirty_`; added regression test.
- **Defect 3 Fixed (`social_simulation.cpp`):** Enforced `max_encounter_distance_m` (default `6.0 m`) in `evaluate_encounter()`; added 10 km separation regression test.
- **Defect 4 Fixed (`social_simulation.cpp`):** Added priority arbitration in `apply_gathering_schedules()` so urgent `Flee`/`Assist`/`Investigate` intents are never overridden; added regression test.
- **Defect 5 Fixed (`persistent_world.cpp`):** Replaced 10-minute wage sampling with exact continuous shift interval overlap arithmetic (`16:59->17:09` = `25 cents`); added regression test.
- **Added Functional Local Economy (`local_economy.hpp/.cpp/_test.cpp`):** Integer-cents (`int64_t`) ledger with exact conservation, attendance-verified wages with duplicate-tick guard, inventory & closures, delivery vehicle dispatch & restocking, economic destination choice, and `PersistentWorldStore` save/load consistency (`3/3` suites).
