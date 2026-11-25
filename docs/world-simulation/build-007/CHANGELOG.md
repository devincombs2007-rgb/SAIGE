# Changelog

## Build 007 (2026-11-25)
- **All Test Functions Invoked (`npc_simulation_test.cpp`):** Fixed `main()` so all 13/13 `npc_simulation` test functions execute (31/31 total across all 4 suites).
- **Physical Business Revenue (`npc_simulation.hpp/.cpp`):** Revenue now accrues at the end of `step()` only when the venue is open, staffed (`active_staff >= staff_required`), and customers are physically within `venue_arrival_radius_m` (`active_customers > 0`).
- **25+ Simultaneous World Events (`npc_simulation.hpp/.cpp`):** Replaced evicting 16-entry ring with sorted active `reacted_world_events` pruned strictly on expiration, eliminating eviction churn at 17+ simultaneous events (verified with 25 simultaneous events).
