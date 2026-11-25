# Physics backend evaluation: Jolt Physics against Bullet, and the architecture it implies

Status: evaluation complete on a prototype; **no engine code depends on either library**. Permanent adoption of any physics library needs owner review (see "Decisions for the owner"). Everything below marked *measured* comes from `tools/physics_eval` (raw numbers in `tools/physics_eval/results.json`); *header-verified* means the capability is present in the fetched source or installed headers but was not exercised by a workload; *background* means it is general knowledge about the libraries that I could not verify from this environment.

## Recommendation

Make **Jolt Physics** the primary dynamic-physics backend, behind a small SAIGE-owned backend interface, and keep SAIGE's existing static collision, queries, capsule character controller and vehicle model in place until the compatibility gates below pass. Do not adopt Bullet as a second backend; keep its adapter in the prototype only as the independence check for the interface.

Why Jolt, from the measurements: it matched or beat Bullet on every workload that stresses the architecture (stable stacks at the cheapest tuning, correct continuous collision detection, bit-identical replay including across thread counts, 2 to 2.8 times faster with four threads, a third of the memory), its default material combination rule is already SAIGE's, and it ships the pieces SAIGE would otherwise have to write (ragdoll with pose driving, a wheeled-vehicle constraint with engine, differential and anti-roll bar, a buoyancy impulse, batched body insertion for streaming, a determinism option, a double-precision build at about 2 to 3 percent cost). Its licence is MIT; Bullet's is zlib; both are permissive.

What it does not settle: Windows/MSVC builds and the CI cost of a vendored library were not tested; fracture/destruction is not provided by either library (authored pieces joined by breakable constraints is the realistic design); and the vehicle workload used a free box, not the SAIGE tire model.

## Method

The same workloads run through one interface (`tools/physics_eval/backend.hpp`) with a Jolt adapter and a Bullet adapter. Build: Release, g++ 13.3, `-ffp-contract=off`, 4-core Xeon 2.1 GHz, 15 GiB, software only (no GPU involved). Jolt v5.3.0 is fetched file by file from `raw.githubusercontent.com` at the pinned tag with SHA-256 verification (`tools/physics_eval/fetch_jolt.py`, manifest `jolt-v5.3.0.sha256`; github.com itself is not reachable from the cloud session, the raw host is). Bullet is the Ubuntu 24.04 package `libbullet-dev` 3.24, which is built without multithreaded task scheduling. Reproduce:

```
python3 tools/physics_eval/fetch_jolt.py /tmp/physics_eval/jolt
cmake -S tools/physics_eval -B /tmp/physics_eval/build -DJOLT_ROOT=/tmp/physics_eval/jolt && cmake --build /tmp/physics_eval/build -j
/tmp/physics_eval/build/physics_eval --json results.json        # add -DJOLT_DOUBLE=ON for the double-precision Jolt build
```

Defaults that differ between the libraries and would have skewed results unless equalized (each is a trap for any integration): Jolt's default linear and angular damping is 0.05 per second (Bullet 0; it removed exactly 18 percent of momentum over 4 s before I zeroed it); Bullet's default sleep threshold is 0.8 m/s, so a body drifting at 0.2 m/s falls asleep after two seconds (a drifting boat would stop); Bullet combines friction and restitution by multiplication, Jolt by geometric mean and maximum (SAIGE's rule); both default to 10 velocity iterations.

## Results

### 1. Stacking: 20 boxes of 20 kg (and a variant with a 200 kg top box), 20 s at 60 Hz *(measured)*

Drift is the largest horizontal displacement of any box at the end; tilt is the top box's. "xN" is substeps per frame, "iM" velocity iterations.

| variant | Jolt drift m / tilt deg | Bullet drift m / tilt deg |
| --- | --- | --- |
| aligned x1/i10 (defaults) | 24.7 / 95.9 (collapses) | 0.30 / 1.6 |
| perturbed (1 cm offsets) x1/i10 | 35.7 / 109.4 (collapses) | 0.044 / 0.1 |
| heavy top x1/i10 | 15.1 / 147.3 (collapses) | 15.7 / 94.2 (collapses) |
| aligned x2/i10 | 0.68 / 2.6 | 0.072 / 0.3 |
| heavy top x2/i10 | 16.4 / 150.9 (collapses) | 0.114 / 0.7 |
| aligned x4/i10 | 0.025 / 0.1 | 0.022 / 0.1 |
| heavy top x4/i10 | 0.21 / 1.0 | 0.034 / 0.1 |
| aligned x1/i30 | 0.014 / 0.1 | 0.129 / 0.5 |
| heavy top x1/i30 | 0.016 / 0.3 | 0.169 / 1.1 |
| heavy top x1/i60 | 0.014 / 0.2 | 0.088 / 0.5 |

Both libraries need tuning for a 20-box stack: at default settings and one step per frame Jolt's stacks collapse and Bullet's heavy-top stack collapses. Jolt reaches rest-quality stability (under 2 cm drift, under half a degree, even with the heavy top) at 30 velocity iterations and **one** step per frame, the cheapest way to buy it; Bullet needs 4 substeps or 60 iterations for the heavy top. A SAIGE integration must therefore set solver effort deliberately (30 iterations is the starting point) and carry this as a regression test.

### 2. Materials *(measured)*

With the SAIGE rule (friction by geometric mean, restitution by the larger; Jolt's default, a callback in Bullet) both match the reference within a few percent on sliding distance (Jolt 1.69 m against 1.73 expected on rubber, 7.32 against 7.36 on ice; Bullet 1.68 and 7.25) and within sampling error on bounce apex at 60 Hz (rubber: Jolt 0.75 m, Bullet 0.73 m, expected 0.74). **Bullet's default rule fails the compatibility test**: its multiplicative combination gives essentially no bounce (0.0008 m on rubber against 0.74 m) and a 42 m slide on ice against 7.4 m. Material-dependent collisions therefore work in Jolt as configured and in Bullet only with a contact callback. SAIGE's `PhysicsMaterial` table maps one to one onto Jolt's per-body friction and restitution.

### 3. Continuous collision detection *(measured)*

A 0.1 m sphere at 300 m/s (5 m per step) and a 0.2 m box at 100 m/s against a 1 cm wall: both libraries tunnel through it without CCD (furthest x 34.9 and 30.0 m, 31.4 and 31.7 m) and both stop at the wall with it (22.2 m; Jolt's linear-cast motion quality, Bullet's swept sphere). Neither tunnelled with CCD in any case tried.

### 4. Articulated chain *(measured)* and humanoids *(header-verified)*

A 10-link chain (2 kg links, swing-twist joints, 45 degree swing) released horizontally for 10 s at 120 Hz: largest joint separation 0.024 m in Jolt, 0.045 m in Bullet; no divergence in either (peak link speed 11.8 and 14.8 m/s); swing within the limit. For humanoids Jolt provides a `Ragdoll` class (skeleton-based, `SetPose` and `GetPose`, driving to a pose with motors or by kinematics: exactly the animation-to-physics transition SAIGE needs); Bullet provides `btMultiBody` (reduced coordinates) and constraint-based ragdolls. The motor-driven path is what active physical animation would use later, as sequenced in the plan.

### 5. Vehicle against movable objects *(measured)* and vehicle integration *(header-verified)*

A 1450 kg body at 20 m/s into 20 crates of 20 kg (a 72:1 mass ratio) on a frictionless floor: momentum is conserved (1.000 in Jolt, 0.9995 in Bullet), no crate exceeds the physically possible speed (19.7 and 20.8 m/s), no instability. Force at a point matches the analytic angular acceleration in both (Jolt 29.98 rad/s^2, Bullet 30.0, analytic 30.0), which is what SAIGE's tire forces would rely on. Jolt has a complete `VehicleConstraint` (wheeled, tracked, motorcycle; engine, differential, anti-roll bar) and Bullet a `btRaycastVehicle`. **Recommendation: keep SAIGE's vehicle model** (its tire, powertrain and damage state are tested and being extended) and use the backend only for the chassis rigid body, wheel ray casts against dynamic and static bodies, and force application, rather than adopting a library vehicle model.

### 6. Determinism *(measured)*

Same build, same inputs: bit-identical state after 360 steps of a 300-box pile in both. Jolt is also identical between 1 and 4 threads. Cross-platform determinism (Jolt's `JPH_CROSS_PLATFORM_DETERMINISTIC`) was not tested: it needs a second platform. Replay determinism holds on one build for both libraries.

### 7. Scaling *(measured)*, box pile dropped into a bin, mean of the 180-step run; the last second is heavier because the pile is still moving

| bodies | Jolt 1 thread | Jolt 4 threads | Bullet 1 thread (no threading in this build) | memory Jolt / Bullet |
| --- | --- | --- | --- | --- |
| 1000 | 3.3 ms | 2.9 ms | 4.3 ms | 1 / 5 MiB |
| 2000 | 10.5 ms | 6.0 ms | 10.4 ms | 4 / 13 MiB |
| 5000 | 38 ms (56 late) | 16 ms (22 late) | 65 ms (109 late) | 14 / 57 MiB |
| 10000 | 90 ms (167 late) | 33 ms (57 late) | 118 ms (214 late) | 33 / 102 MiB |

Four threads give Jolt a 2.3 to 2.8 times speedup once there is work; Bullet's packaged build cannot use them (a self-built Bullet can, with `BT_THREADSAFE`, not tested). A 60 Hz budget of roughly 4 ms for physics fits about 1000 simultaneously active bodies on one thread and about 2000 on four; sleeping bodies cost almost nothing (100 and 500 box piles fell asleep in the run).

### 8. Large worlds *(measured)*

A resting box and a 5-stack keep exact height and no jitter at any distance in all builds, because the vertical axis is small. **Lateral precision does not hold in float builds**: a slow slide (0.2 m/s, 1.0 m in 5 s) is accurate to 5 km in float Jolt (1.025 m), off by 17 percent at 20 km (1.17 m), and stuck at 100 km (0 m); Bullet in float stops at the same 100 km. The double-precision Jolt build is exact (1.000 m) at every distance, with scaling cost within 2 to 3 percent of the float build at 10000 bodies (93 against 90 ms single-threaded). The current SAIGE rule (chunk-local frames within +-256 m, host rebases) is well inside the float range and needs no change; a double build is the option if streaming ever needs one continuous coordinate frame beyond about 5 km.

### 9. Features not exercised by a workload

Buoyancy: Jolt has `Body::ApplyBuoyancyImpulse` (surface position and normal, buoyancy, drag, fluid velocity: it matches the water module's needs) *(header-verified)*; Bullet needs hand-written forces. Destruction: neither provides fracture; both provide constraint breaking (Bullet `setBreakingImpulseThreshold`; Jolt constraint force limits and `GetTotalLambda*`) *(header-verified)*. Streaming: Jolt's `AddBodiesPrepare` and `AddBodiesFinalize` insert many bodies without stalling the simulation thread *(header-verified)*. Soft bodies exist in Jolt *(header-verified)*. Maintenance: Jolt is under frequent development with many releases and wide production use, Bullet's last packaged release is 3.24/3.25 (2022 and 2023) *(background; not verifiable from here)*.

## Proposed architecture

```
Game / animation / vehicle / water / ragdoll (SAIGE code)
        |  uses only
saige::PhysicsWorld (SAIGE-owned interface; shape: tools/physics_eval/backend.hpp)
        |  adapters, selected at build time (CMake option, off by default)
JoltBackend (primary)          NullBackend (tests, headless)          [any other, e.g. Bullet, for independence checks]
```

What stays SAIGE-owned and does not move into the library: `StaticCollision` (the streamed static world, queries and the surface map), the capsule character controller (collision-and-slide, steps, crouch, drive), the vehicle model (tires, powertrain, damage), the physical material table and combination rule, the water module. What the backend adds: dynamic rigid bodies and their contact manifolds, stacking, constraints and joints, CCD, ragdolls, multithreaded stepping, and body-versus-character and body-versus-vehicle contacts. The interface starts as the prototype's (`add_box`/`add_sphere`, swing-twist and hinge joints, velocity, force at a point, position and rotation, ray cast, steps with substeps, solver effort, thread count, materials); it grows only as a consumer needs it, and a shared header under `include/saige` is proposed on the issue first (Core owns shared headers).

## Compatibility gates before any existing physics is replaced or extended with the backend

1. **Material parity**: SAIGE's `PhysicsMaterial` table and `SurfaceMap` map to backend materials; the bounce and slide checks above pass in an engine test with the backend, using SAIGE's rule.
2. **Static world parity**: `StaticCollision` boxes and triangles are mirrored into the backend as static bodies or mesh shapes, and a ray cast through both agrees (hit point within 1 mm), so the character controller and vehicle keep their current ground truth.
3. **Character against bodies**: the capsule controller pushes and is blocked by dynamic bodies through a defined contact exchange (character as a kinematic body for the backend; bodies as obstacles for the capsule); no change to the controller's behaviour in a world with no dynamic bodies (bit-identical replay).
4. **Vehicle against bodies**: the SAIGE vehicle's chassis is a backend body; wheel rays query static and dynamic bodies; tire forces apply as force at a point; with no dynamic bodies the existing vehicle tests and the Showcase 03 replay checks pass unchanged.
5. **Determinism**: replay identical on one build, 1 and N threads, in the engine test suite; cross-platform determinism decided by the owner (needs a second platform and the Jolt flag).
6. **Stability and cost workloads** from this evaluation become engine regression tests (stack, CCD, chain, scaling budget).
7. **Windows**: the backend builds and passes on MSVC in CI before it is enabled anywhere.

## Phased plan (each phase is reversible and behind an off-by-default option)

* **Phase 0 (done)**: this evaluation, the prototype, the pinned fetch.
* **Phase 1**: an `engine/physics_backend` module with the interface, a `NullBackend` and the Jolt adapter behind `SAIGE_PHYSICS_JOLT` (off by default; source fetched and hash-checked, not vendored into the repository); the gates 1, 2, 5, 6 tests. Owner review of the dependency is the gate to start this phase.
* **Phase 2**: character and vehicle contact exchange (gates 3, 4); movable props in a test world.
* **Phase 3**: ragdolls: stable joints, collision and anatomical limits first, then animation-to-physics transition (switch a humanoid from clip to ragdoll and back with a blend), using Jolt's ragdoll class or the same joints through the interface.
* **Phase 4**: buoyancy and drag from the water module applied to dynamic bodies; breakable-constraint destruction of authored pieces; active physical animation only after Phase 3 stability is demonstrated.

## Decisions for the owner

1. Approve Jolt (MIT, vendored by pinned fetch, not committed) as the primary backend candidate for Phase 1, or name different criteria. Until then nothing in the engine links it.
2. Float with chunk-local frames (current rule, sufficient to about 5 km) or the double-precision build for a single continuous world frame.
3. Whether cross-platform determinism is a requirement (it costs a Jolt flag and a second-platform test).
4. A Windows CI job that builds the prototype, to resolve the one unknown (MSVC) before Phase 1.
