# Large-world precision (PREC-1)

**Decision: no second coordinate strategy.** SAIGE's existing architecture is already the right one: simulation and rendering run in a single-precision frame local to a chunk (the physics envelope is +-256 m, `capsule_sweep.hpp`) and
move between frames by **origin migration** (`CapsuleCharacter::migrate_origin`, `Vehicle::migrate_origin`, `DynamicsWorld::shift_origin`, the navigation map's rebasing); the global place of anything is a chunk cell (`ChunkCoord`, 32-bit
integers) plus its local offset. `include/saige/large_world.hpp` names that pair (`WorldPosition`) and does the arithmetic that must be in double: converting to and from a global double position, the vector between two positions
(from the cell difference, which is exact), the position in another cell's frame, advancing across borders, and the shift to hand to a migration. No code path adds large floats.

(`ChunkCoord` moved from `chunk_stream.hpp` to `chunk_coord.hpp`, which `chunk_stream.hpp` includes: source compatible, no migration needed for consumers. Core owns the header; this is a pure move.)

## Measurements (`tests/large_world_test.cpp`, cell size 256 m)

| Distance from the origin | Resolution of a float world position | Resolution of the local part |
| --- | --- | --- |
| 1 km | 61 micrometers | 15 micrometers |
| 10 km | 0.98 mm | 15 micrometers |
| 100 km | 7.8 mm | 15 micrometers |
| 1000 km | 6.3 cm | 15 micrometers |
| 10,000 km | 1.0 m | 15 micrometers |

* Round trip global double -> cell + float -> global double, to 1e9 m, 20,000 random positions: worst error 7.6 micrometers (half a float step of the local part), independent of distance.
* Cell borders: positions on a border are the next cell at offset 0; negative coordinates floor; an offset that rounds to the cell size carries into the next cell; positions within an ulp under a border at 1e6 to 2e9 m always get a valid offset and round-trip to 16 micrometers.
* Two points 1 mm apart across a cell border at 100 km and at 10,000 km: `difference` gives 1.004 mm (the 4 micrometers is the local float step); subtracting float world positions gives exactly 0, 1 mm lost.
* Origin migration of a driving vehicle: the same 17 s drive (190 m) with and without three migrations of 64 m (a power of two, exact in float): the end position differs by 2.4 micrometers.
* The dynamics backend crossed an origin shift of (60, -35) m after 100 steps with 5.5e-6 m difference in single precision and 2.6e-6 m in Jolt's double configuration (`docs/physics-backends.md`).

Rules that follow: use power-of-two cell sizes (exact shifts); never subtract two world floats; save and stream positions as cell + local; shift whole simulations by `migration_shift`, not by recomputing positions.

## Jolt's double-precision configuration, evaluated separately

`SAIGE_JOLT_DOUBLE=ON` (Jolt `JPH_DOUBLE_PRECISION`) passes every dynamics gate, so it works, but it is not needed by this architecture: Jolt worlds live in a chunk-local frame where single precision resolves 15 micrometers or better.
It costs memory and speed in every body transform and broadphase box, and changes bit patterns of saved state. Reasons that would justify it later: a single physics world that must span more than a chunk (one continuous rigid-body
simulation across a 100 km road without shifting). Until a workload shows that, single precision stays the default and double stays a CI-verified option. No benchmark of the double build's cost has been run (not established).

## Not done

* Precision of rendering at distance (camera-relative rendering of far chunks, depth precision): Graphics' side; the camera relative position should come from `relative_to`, not from float world positions.
* Streaming large worlds with saved positions in the project format (`WorldPosition` is not yet in the versioned chunk manifest/checkpoint formats; the existing `CourseCheckpoint` is already cell + local).
* Migration policy (when to migrate, hysteresis) for the dynamics world and for pedestrians across streamed borders; navigation across streamed boundaries works within one frame today (`docs/navigation.md`).
