# Engineering roadmap (dependency-ordered)

This refines `docs/architecture.md` ("Production roadmap"). Order is by dependency, not by calendar; each line names what it needs. Status words are used strictly: **implemented** (code exists on a branch),
**tested** (its tests pass locally), **CI-validated** (CI green on the exact head), **merged** (in `main`), **hardware-validated** (run on a physical GPU or the target device). See `capabilities.md` for the
current state of each.

| # | Track | Next increment | Needs | Exit evidence |
| --- | --- | --- | --- | --- |
| 1 | Integration | Land the stacked render and physics PRs into `main` in dependency order (see `development-state.md`) | owner approval per major merge | CI green on each exact head, rebased onto the proposed base |
| 2 | Physics | Physical material table (friction, restitution, density) shared by character, vehicle, debris; vehicle model upgrade (engine and gearbox curves, differential, tire load sensitivity, damage state); evaluate Jolt and Bullet behind a physics backend interface | 1 (collision queries merged), owner approval before adopting a library | determinism test over a replay, comparison against the current vehicle on a fixed course |
| 3 | Characters and animation | Humanoid import of the CC0 libraries, animation state machine, root motion, foot IK, retargeting, ragdoll to active ragdoll | `include/saige` animation contract agreed with Core (issue), skinning merged | clip playback parity against the CPU reference, a gait that follows speed without foot sliding |
| 4 | Core architecture | Entity storage and job system under measured workloads; project and asset versioning with tested migrations | instrumentation (perf PR #68) | before and after numbers on representative scenes |
| 5 | Assets and tools | Catalog growth, automated license check in CI, reproducible tool setup on Windows | `tools/production`, `assets/catalog` | `catalog validate` in CI |
| 6 | World simulation and AI | Navigation over streamed cells, schedules, traffic | streaming (Core), physics 2 | deterministic replay of a day |
| 7 | Graphics | Quality presets, point lights, capability-gated ray tracing and upscalers behind the backend boundary | 1 | per-feature capability report; never reported operational without hardware |
| 8 | Audio | Production runtime architecture (miniaudio is a proof of concept: Doppler, pan, attenuation, occlusion, reverb, RPM blend verified) | owner approval for a dependency | event-driven mix under the simulation clock |
| 9 | Platform | Windows bootstrap and qualification scripts, physical GPU runs | owner's Windows machine | qualification report with adapter classification |

Vulkan 1.4 is opt-in with a 1.3 baseline (draft PRs #44 and #45); C++ standard, compiler matrix and shader tools are audited by `tools/audit/` (see backlog).
