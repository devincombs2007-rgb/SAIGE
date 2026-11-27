# World Simulation Build 010: integration of the four new modules (2026-11-27)

Received in the Gemini engineering transfer (`upstream/gemini/PROVENANCE.md`; the delivered files are kept unchanged in `upstream/gemini/world_simulation_build010/`).
Owner directive 7209812710 makes Claude the maintainer.

## What Build 010 contained and what was integrated
| Module | State on receipt | Action |
| --- | --- | --- |
| `crowd_avoidance`, `multi_floor_nav`, `npc_simulation`, `traffic_simulation`, `saige_world_adapter` | byte-identical to the copies already on every live branch, with identical tests | nothing to do |
| `persistent_world` | new | integrated into `saige_core` with fixes (below) |
| `local_economy` | new | integrated with fixes (below) |
| `npc_psychology`, `social_simulation` | new | integrated as delivered |

Tests: the four delivered test programs (`tests/persistent_world_test.cpp`, `npc_psychology_test.cpp`, `social_simulation_test.cpp`, `local_economy_test.cpp`) and a new
`tests/world_sim_regression_test.cpp` (42 checks) are registered in the root `CMakeLists.txt`. All pass, also under AddressSanitizer and UndefinedBehaviorSanitizer.
Mutation: 5 of 5 mutants of the fixes are caught by the regression test. The four delivered modules also compile without a warning under the graphics lane's strict set
(`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wdouble-promotion ... -Werror`).

## Defects found on receipt and fixed (each measured on the delivered code with a probe)
| # | Defect (Build 010) | Measured | Fix |
| --- | --- | --- | --- |
| 1 | Wages were paid as `max(1, round(dt / 60 * wage))` cents per tick: the pay depended on the tick rate, and a zero wage paid a cent a tick | one minute at 25 c/min paid 60 cents at 1 Hz and 3600 cents at 60 Hz; a zero wage paid 100 cents in 100 ticks | fractions of a cent accrue per contract and whole cents are paid; unpaid wages stay owed. Now 25 cents at 1, 10, 60 and 144 Hz, 250 cents for ten minutes, 0 for a zero wage |
| 2 | `quantity * unit_price` and `units * wholesale_cost` were unbounded `int64` products | a price of 2^62 registered; buying 4 overflowed (UBSan) and the purchase **succeeded with a negative cost, creating money** | bounded amounts (`kMaxUnitPriceCents`, `kMaxAccountCents`, `kMaxPurchaseUnits`, `kMaxInventoryUnits`, `kMaxWagePerMinuteCents`) refused at registration; a new `PurchaseStatus::InvalidQuantity`; a till that would exceed its bound refuses the sale before anything changes |
| 3 | Registration accepted nonsense | opening minute 5000, cash -100 and stock 500 above a maximum of 10 were accepted | refused, with shift minutes and reorder thresholds also checked; a negative treasury throws |
| 4 | Delivered stock was summed in 32 bits before the clamp | (wrap possible near the 32-bit limit) | summed in 64 bits |
| 5 | The store accepted any value | a NaN position, hunger 7, an infinite affinity and an event radius of -5 were stored | one validation per record kind, used by the upserts and the loader; enum values are checked on load |
| 6 | A save dropped the ledger: business cash and the supplier treasury were not persisted | 125000 cents before a save, **202600** after loading it | save format **v3**: business cash and expenses and the economy treasury; `sync_to_persistent_store` and `load_from_persistent_store` carry them; now 125000 before and after |

Loading is stricter: a save must be consumed exactly (no trailing bytes), its time finite, its records valid and its identifiers unique, or it is refused and the store is
unchanged (20000 random corruptions of a save were all refused by the CRC, with no change to the store, before and after these changes).

## Save format compatibility
- **v3** is written. **v2** (Build 009 and 010) and **v1** (Build 008, `WLD1`) still load.
- A real v2 save written by the delivered Build 010 code is embedded in the regression test (`kBuild010V2Save`, 385 bytes): it loads with every field, has no till and no
  treasury (v2 never had them), and re-saved as v3 it round-trips byte for byte.
- The v1 path used to copy `sizeof(BusinessRecord)` bytes per business, which would have read every v1 save with whatever layout `BusinessRecord` had become. v3 added fields to
  `BusinessRecord`, so the Build 008 business layout is now pinned (`V1BizRecord`, 64 bytes, a `static_assert`). The v1 path still copies raw structures, which ties v1 saves
  to the x86-64/AArch64 layout; there is no Build 008 save in the repository to test against, so this is recorded, not changed.

## Not done
- `npc_psychology` and `social_simulation` were integrated after their own tests and the sanitizers; I have not reviewed their logic line by line.
- Nothing consumes the four modules yet: they are not wired into the runtime, the Showcase code or the navigation and traffic of the live world. That reconciliation
  (exact ledger under the live simulation, event priority, crowd and navigation behaviour at chunk boundaries) is the next step for this division.
- The delivered `crowd_avoidance` test takes about 26 seconds in a Debug build.
