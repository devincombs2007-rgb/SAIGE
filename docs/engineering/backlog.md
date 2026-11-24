# Backlog (dependency-ordered; top of each group first)

## Defects and debt

* Barrel stack prop is non-solid (multi-piece model); give it a compound hull.
* The unlit window material is a flat dark glass; lit windows have no interior and light nothing around them.
* Wheel-of-a-wagon aliasing at 30 fps; add temporal supersampling (average sub-frames) to the offline capture path before any further film.
* Brake pads are drawn rigidly with the wheel and so spin with it; mount them on the hub.
* `production_tools_python` skips when the tools venv is not at `$SAIGE_TOOLS_HOME` or `~/.saige-tools`; make CI create it with `tools/production/setup.sh` so the test runs rather than skips.

## Physics (22%)

1. Physical material table shared by character, vehicle, debris.
2. Vehicle: engine and gearbox curves, differential, tire load sensitivity, damage state, handbrake and drift tests.
3. Rigid-body stacking and joints; evaluate Jolt and Bullet (license, determinism, maintenance) behind a backend interface; owner approval to adopt.
4. Water (buoyancy, drag), destruction (fracture of authored meshes).

## Characters and animation (20%)

1. Replace CesiumMan: humanoid import of the CC0 libraries (Quaternius UAL1 and UAL2, KayKit), retargeting, locomotion state machine, root motion.
2. Foot IK and ground adaptation, look-at, hand IK for vehicle entry.
3. Ragdoll, then active ragdoll toward physical animation.

## Core architecture (21%)

1. Replay the Showcase 03 workload under the perf instrumentation and record the entity-storage and threading baselines before changing either.
2. Project and asset format versioning with migrations.

## Assets and tools (13%)

1. `catalog validate` in CI; license matrix check.
2. Windows bootstrap and qualification scripts.
3. Toolchain and dependency audit: done as `tools/audit` (offline; policy in `policy.json`). Next: run it in CI and on the Windows machine, and record the result per release candidate.

## World simulation and AI (10%), graphics (7%), audio (7%)

* Navigation mesh over streamed cells; schedules; traffic.
* Quality presets; point lights; capability-gated RT and upscalers.
* Production audio runtime evaluated against the miniaudio proof of concept.

## Automation

* A bounded, single-writer, checkpointed scheduled engineering run (see `agent-handoff.md`). Not enabled until configured and verified.
