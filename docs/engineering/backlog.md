# Backlog (dependency-ordered; top of each group first)

## Defects and debt

* Barrel stack prop is non-solid (multi-piece model); give it a compound hull.
* The unlit window material is a flat dark glass; lit windows have no interior and light nothing around them.
* Wheel-of-a-wagon aliasing at 30 fps; add temporal supersampling (average sub-frames) to the offline capture path before any further film.
* Brake pads are drawn rigidly with the wheel and so spin with it; mount them on the hub.
* `production_tools_python` skips when the tools venv is not at `$SAIGE_TOOLS_HOME` or `~/.saige-tools`; make CI create it with `tools/production/setup.sh` so the test runs rather than skips.

## Physics (22%)

1. Physical materials: table, vehicle tires and vehicle body contacts done (`docs/physics-materials.md`); character traction is done (`CapsuleInput::traction`, derived by the host from the surface under the feet: no change to what the controller reports); next are debris restitution and a Showcase world that builds a `SurfaceMap` from its surface enum (opt-in, since it changes the scripted drive).
2. Vehicle: engine and gearbox curves, differential, tire load sensitivity, damage state, handbrake and drift tests.
3. Rigid-body stacking and joints; evaluate Jolt and Bullet (license, determinism, maintenance) behind a backend interface; owner approval to adopt.
4. Water (buoyancy, drag), destruction (fracture of authored meshes).

## Characters and animation (20%)

1. Done (`docs/animation-character.md`): humanoid map, BVH import and loop extraction, retargeting, locomotion state machine with stride-matched playback, ground and wall foot IK, capsule glue, real UAL and CMU assets.
2. Done: foot locking (`lock_planted_feet`): the retargeted capture cycles went from 10 to 27% to 0.1 to 4.8% of the speed. Next: apply the pelvis correction when a lock pulls a leg to its limit, and put lock and ground IK behind one call on the controller.
3. Replace CesiumMan in Showcase-class apps with the UAL rig and this controller (not Showcase 03: it stays as delivered); upload the UAL mesh through gfx_scene skinned staging.
4. Strafing and turn in place; crouch; a root-motion mode for clips that carry the character (roll, climb) with the capsule taking the clip's delta as a movement request; upper-body layering (aim, carry); look-at; hand IK for vehicle entry; a second jump.
5. Ragdoll, then active ragdoll toward physical animation.
6. A speed ladder agreed with the animator: the UAL has a walk at 1.05 m/s and a sprint at 8.96 m/s and no jog; add a jog and check the capsule's walk and sprint speeds against the cycles.

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
