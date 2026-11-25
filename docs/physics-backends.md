# Dynamic physics backend (DYN-1)

SAIGE owns its physics interface; a library sits behind it. `include/saige/dynamics.hpp` defines `DynamicsWorld` (bodies, constraints, kinematic targets, impulses, ray casts, origin shifts) in SAIGE's types
(`DVec`, `DQuat`, `Surface` and the physical material table, `StaticCollision`). Nothing in it names a vendor. `make_dynamics_world(DynamicsBackend::Jolt, config)` returns null when the backend is not compiled in.

What stays SAIGE's own and is **not** replaced by the library: the capsule character controller, the raycast vehicle (differential, tires, damage), the water module and the physical material table. The backend is for what they
do not do: free rigid bodies (crates, debris, ragdoll limbs), stacking, joints. Characters and vehicles meet it as kinematic bodies (one-way coupling for now: they push dynamic bodies, which do not push back).

## Build

Off by default (`-DSAIGE_PHYSICS_JOLT=OFF`). To build it:

    python3 -I tools/physics_eval/fetch_jolt.py DIR --tag v5.7.0
    cmake -S . -B build -DSAIGE_PHYSICS_JOLT=ON -DSAIGE_JOLT_ROOT=DIR [-DSAIGE_JOLT_DOUBLE=ON]

`fetch_jolt.py` downloads each file from the raw host at the tag and checks it against `tools/physics_eval/jolt-v5.7.0.sha256` (537 files, SHA-256). Jolt is MIT licensed. No Jolt source is committed.

**Pin: v5.7.0.** Verified as the latest release tag reachable from the cloud session by probing the raw host for v5.3.0 through v5.8.0 and v6.0.0 (v5.7.0 exists, later ones do not, and `master` reports 5.7.0 in its version header).
The earlier evaluation used v5.3.0; the harness numbers there are not re-measured on v5.7.0 (backlog).

## Status and version policy

**Jolt is SAIGE's permanent dynamic physics dependency** (owner and development director, 2026-11-25), always behind `DynamicsWorld`; the character controller, vehicle, water and material table remain SAIGE's own. The rule for versions: the newest stable
release that passes the acceptance tests (`dynamics_test`, `ragdoll_test`, `buoyancy_test`, the tower gate below, ASan/UBSan, ThreadSanitizer, double precision, MSVC). **Pinned: v5.7.0**, the newest tag that exists (probed to v6.0.0) and passes. The adapter needs at least
v5.4.0 (`HingeConstraint::SetTargetOrientationBS`); v5.4.0, v5.5.0, v5.6.0 and v5.7.0 all pass the whole `dynamics_test` except the pin check, and v5.3.0 does not build the adapter. No older version needs to be retained.

### The stacking change between v5.5.0 and v5.6.0 (investigated)

Bisected with the same harness on v5.3.0 to v5.7.0 (`tools/physics_eval`, `stacksweep` workload: 55 configurations of column height, substeps, velocity and position iterations): **v5.3.0, v5.4.0 and v5.5.0 are bit-identical; the change first appears in v5.6.0** and v5.7.0 is the same as v5.6.0.
It is **not a changed default** (`PhysicsSettings` is identical apart from an unrelated internal-edge tolerance), **not our solver settings**, and **not an upstream defect**: v5.6.0 replaced Jolt's friction model. Up to v5.5.0 every contact point of a manifold carried
its own pair of friction constraints (`wcp.mFrictionConstraint1/2`, limited by that point's normal impulse); from v5.6.0 a manifold has one pair of friction constraints at the centroid of its contact points plus a new angular (torsional) friction constraint limited by the
normal impulses times their distances to that centroid (`ContactConstraintManager.cpp`, `AngularFrictionConstraintPart.h`). Evidence that the new model is sound and the old stack was at a margin: disabling the angular friction in v5.7.0 makes the 20-box column far worse (25 m of drift),
and the old model is itself non-monotonic in solver effort (a 20-box column at one substep stands with 30 velocity iterations, falls with 45, stands with 60 and 90).

What changed in practice, measured (Linux GCC, Release, single precision, shared CPU: costs indicative):

| Column | v5.5.0 | v5.7.0 |
| --- | --- | --- |
| 10 boxes, 1 substep, 10 velocity iterations | stands, drift 1.7 cm, asleep | stands, drift 5.2 cm, asleep (3 times the creep, still within what a person calls standing) |
| 15 boxes | stands at 1 substep from 30 iterations, at 2 from 60 | identical |
| 20 boxes, 1 substep | stands at 30, 60, 90 iterations (not 45) | **falls at 30 and 45**, stands at 60 and 90 |
| 20 boxes, 2 substeps | stands from 45 iterations | identical |
| 20 boxes, 4 substeps | stands at every effort tried | identical |
| Cost, 20 boxes, 1 substep, 60 iterations | 0.0042 ms per step | **0.0024 ms** (a fall to rest 1.8 times cheaper) |
| Cost, 20 boxes, 2 substeps, 60 iterations | 0.0213 ms | **0.0085 ms** |
| Cost, 20 boxes, 4 substeps, 60 iterations | 0.0588 ms | **0.0332 ms** |

So the regression is narrow: one marginal configuration of a 20-metre column at one substep and 30 iterations, while the solver is about 1.8 times cheaper on the configurations that stand. Nothing was fixed by "more effort" blindly: a 20-box tower is stable at **60 velocity iterations
and one substep (0.0024 ms per step)** or two substeps, and that is what the gate pins. `DynamicsConfig` defaults stay at 10 iterations and one substep (a 10-box column stands and sleeps); tall towers and ragdolls choose their own (the ragdoll tests use four substeps, 20 velocity and 10 position
iterations, measured in `docs/ragdoll.md`).

**The gate** (`dynamics_test towers`): ten boxes at the defaults; fifteen at two substeps and 30 iterations; twenty at two substeps and 60; twenty at one substep and 60: each must stand (drift under 8 cm, tilt under 1 degree) and be asleep after 20 s. It passes on v5.4.0 to v5.7.0, so a future upgrade
that breaks stacking is caught by CI, not by a game.

### Lifetimes, threads and sanitizers

`dynamics_test lifetimes` churns 3000 steps on four workers (1,328 bodies created, 1,029 removed, 440 constraints, slots reused with a new generation 3,082 times; every removed id stays refused; the live count always equals the test's; the state stays finite). It found a defect in
the adapter: self-collision sub-group ids were never returned when a body was removed, so a group refused its 65th body ever, not its 65th live body. Fixed (ids are returned and reused; mutation-checked). The whole of `dynamics_test` and `ragdoll_test` also run clean under ThreadSanitizer
(Jolt's job system with 4 workers) and under ASan/UBSan, and TSan is a CI job.

## Compatibility gates (`tests/dynamics_test.cpp`)

| Gate | What it measures | Result (Jolt 5.7.0, Linux GCC, single and double precision) |
| --- | --- | --- |
| API | stale ids refused, slot reuse bumps the generation, NaN/zero-size/negative-mass bodies and bad configs rejected | pass |
| Static collision parity | 400 random rays through `StaticCollision::raycast` and the backend over boxes plus two-sided triangles: hit/miss, fraction, normal | 0 disagreements, fraction within 1e-7, normals identical (rays starting inside a solid box excluded: StaticCollision reports distance 0, Jolt a face) |
| Resting heights | boxes dropped on the floor and on three box tops stop at the surface height, to the contact slop | within 3 cm (the backend's penetration slop) |
| Materials | sliding acceleration on a 20 degree ramp for asphalt, gravel, ice follows `g (sin - mu cos)` with mu the geometric mean of body and surface friction; rebound of a sphere on concrete and rubber follows the larger restitution squared; an explicit friction overrides the table | ice 1.759 against 1.759 expected; asphalt and gravel stick as predicted; bounce apex 0.050 against 0.040 and 0.430 against 0.422 |
| Stacking | 5 and 10 boxes for 10 s, a 15-box pyramid | all still standing, drift under 3 mm, asleep |
| Constraints | hinge (pivot error, pendulum bottom, limit angle), swing-twist cone, fixed, distance (rope), chain of ten links | hinge pivot error 4.5 mm; limit 0.500 rad; cone 0.552 rad against 0.5 for a 10 m/s blow and 0.709 for a 40 m/s blow (overshoot is reported, see below); rope 2.000 m; chain 4.469 m against 4.5 |
| Continuous collision | a 143 m/s ball against a 5 cm wall | discrete: tunnels (x = 140.8); continuous: stops (x = 9.96) |
| Deterministic replay | an 80-box pile twice, bit-identical after 150 steps; one step fewer differs | identical; one thread against four also identical here, but not promised by the build (see below) |
| Origin migration | the same 12-box pile with the origin shifted by (60, -35) after step 100 | 5.5e-6 m difference |
| Interactions | a character-sized kinematic capsule walking at 3 m/s pushes a 20 kg crate; the SAIGE vehicle at 15 m/s drives into five crates | crate carried at 3.6 m/s; fastest crate 17.9 m/s (an elastic hit by an immovable body allows at most twice the car's speed) |

Mutation checks against the adapter (each killed by at least one gate): arithmetic instead of geometric friction mean, min instead of max restitution, one-sided triangles, wrong body order for world-fixed constraints,
un-flipped ray normals, continuous flag ignored, explicit friction ignored, one velocity iteration, bodies not moved by `shift_origin`.

## Differences found while writing the gates (kept, not hidden)

* **Triangles are two-sided in SAIGE, one-sided in Jolt's simulation** (`PhysicsSystem` ignores back faces for triangles). The adapter adds each triangle in both windings. The first test run had bodies falling through a reverse-wound ramp.
* **Ray normals** face the ray origin in SAIGE; the adapter flips Jolt's geometric normal to match.
* **Damping**: Jolt's default linear/angular damping is 0.05; SAIGE bodies default to 0 (`DBodyDesc`), so energy and friction results are comparable with the SAIGE models.
* **Contact rules**: friction is the geometric mean, restitution the maximum (set explicitly with `SetCombineFriction/Restitution`), as in `physics-materials.md`.
* **Hinge angles** are those of body `a` about the axis when there is no second body (the adapter puts the world first); with two bodies, those of `b` relative to `a`.
* **Violent constraint blows**: a swing-twist cone is exceeded by 0.2 rad by a 40 m/s blow at 120 Hz. The correction needs more than a step. Ragdolls must keep limb speeds or substeps in a range where this does not matter, or use `collision_steps`.
* **Determinism**: Jolt promises cross-platform determinism only with `JPH_CROSS_PLATFORM_DETERMINISTIC`, which this build does not enable; `info().deterministic_across_threads` reports false. What is tested and required: the same build, same inputs and same thread count give bit-identical results. Cross-platform replay (Windows against Linux) is **not** established and will need that option and its cost measured.

## Interface additions since the first gates (ragdoll support)

`add_angular_impulse`, `set_transform`, `wake`, `remove_constraint`, constraint ids (`add_constraint` returns one), joint motors (`set_motor`/`clear_motor`: a spring-damper toward the second body's orientation relative to
the first, Jolt's motor springs), self-collision groups (`DBodyDesc::self_collision_group`, up to 64 bodies) and `DynamicsConfig::allow_sleeping`. Found while testing: a motor or its removal does not wake a sleeping body (the
adapter wakes both), and a motor's stiffness follows the *joint's* reduced mass, not the load above it (`docs/ragdoll.md`). Gates: `dynamics_test controls` plus the ragdoll tests; each adapter path is mutation-checked.

## Double precision

`-DSAIGE_JOLT_DOUBLE=ON` builds Jolt with 64-bit positions (`JPH_DOUBLE_PRECISION`). The whole gate suite passes in that configuration as well (parity within 1e-7, origin shift 2.6e-6 m). SAIGE's large-world strategy is global
double coordinates with chunk-local single-precision simulation (`world.hpp`, origin migration); the backend sits inside that chunk-local frame, so single precision is the default and double is only a CI-verified option, not a second
coordinate strategy. If a physics world ever had to cover more than a chunk, that decision (and the measured cost) would be revisited in the precision work (PREC-1).

## Not yet done

* Windows MSVC: the workflow `physics-jolt.yml` has the job; it has not run yet. Until it passes the backend remains optional on Windows.
* Re-run of the evaluation harness (Jolt against Bullet) on v5.7.0; stacking, constraint and CCD gates are SAIGE's own, the harness is a performance comparison.
* Two-way coupling of the character and the vehicle (dynamic bodies pushing back), ragdolls, vehicle-body collision with the backend's bodies, performance workloads (numbers on shared CI runners are not engine performance).
* GPU or workstation performance: nothing here is a GPU result; these are CPU tests on this machine and on CI runners.
