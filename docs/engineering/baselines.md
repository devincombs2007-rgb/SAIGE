# CPU baselines of the simulation pieces

Measured with `saige_sim_baseline` (`tools/benchmarks/sim_baseline.cpp`, Release, single thread, no GPU, median of five runs). These are CPU numbers on one cloud machine: **Intel Xeon @ 2.10 GHz (container, 4 shared cores), GCC 13, `-O3`-class Release (NDEBUG)**.
They are the reference a later change to entity storage, threading or a physics backend (see `physics-backend-evaluation.md`) must be compared with *on the same machine*; they say nothing about rendering or about any physical GPU.
Memory was not measured in this pass.

| Operation | ns per call | Per second of simulation at 60 Hz | Notes |
| --- | --- | --- | --- |
| `Vehicle::step` (1/60 s), defaults, flat ground | 3,471 | 0.21 ms | two 1/120 s substeps, four wheel probes, chassis overlap |
| `Vehicle::step`, differential + slip-angle tires + engine braking + damage + powertrain | 3,575 | 0.21 ms | the options cost about 3% |
| `Vehicle::step`, defaults, 2,000 boxes in the world | 3,900 | 0.23 ms | broadphase makes the field nearly free |
| `Vehicle::step`, all options, 2,000 boxes | 4,142 | 0.25 ms | |
| `CapsuleCharacter::step` (1/60 s), 400 boxes | 27,416 | 1.6 ms | collide-and-slide, step-up, ground probe: the most expensive per-agent step here |
| `WaterSurface::height`, 12 waves, choppiness 0.4 | 1,342 | | about 90 transcendental calls (the inversion of the displacement dominates) |
| `WaterSurface::normal`, same | 1,733 | | |
| `FloatingBody::step` (1/60 s), 216 cells, 12 waves | 1,133,286 | 68 ms | 1.1 ms per body per frame |
| `FloatingBody::step`, 1,728 cells, 12 waves | 8,635,907 | 518 ms | 8.6 ms per body per frame |

Findings.

* A vehicle is cheap (about 0.02% of a 16.7 ms frame); a character is about 0.16%; both are far from a budget concern at crowd sizes of tens.
* A **floating body is not**: at 216 cells and 12 waves it costs 1.1 ms of a 16.7 ms frame, so about ten of them is the whole frame. It was 1.9 ms before two changes made while measuring (a fused height-and-flow query `WaterSurface::sample`, which shares the inversion and skips the flow for dry cells; a displacement tolerance of 1e-8 m
  instead of 1e-10 m). The remaining cost is per-cell wave evaluation. The next steps, in order of benefit: fewer waves for bodies (a sea's long waves matter for heave, short ones only for the waterline), sampling the surface per column instead of per cell for bodies that are nearly level, and a surface grid cache per frame shared by all bodies
  in an area. Nothing was changed beyond the first two without a workload that needs it (the directive: instrument before changing fundamentals).
* Debug builds are slower by an order of magnitude (the water test takes 20 s in Debug); do not read the tests' runtimes as costs.

Reproduce: `cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF && cmake --build build-release --target saige_sim_baseline && build-release/saige_sim_baseline` (`--json` for a machine-readable table).
