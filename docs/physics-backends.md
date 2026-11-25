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
