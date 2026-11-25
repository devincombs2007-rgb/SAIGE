# Backlog (dependency-ordered; top of each group first)

## Defects and debt

* Barrel stack prop is non-solid (multi-piece model); give it a compound hull.
* The unlit window material is a flat dark glass; lit windows have no interior and light nothing around them.
* Wheel-of-a-wagon aliasing at 30 fps; add temporal supersampling (average sub-frames) to the offline capture path before any further film.
* Brake pads are drawn rigidly with the wheel and so spin with it; mount them on the hub.
* `production_tools_python` skips when the tools venv is not at `$SAIGE_TOOLS_HOME` or `~/.saige-tools`; make CI create it with `tools/production/setup.sh` so the test runs rather than skips.

## Physics (22%)

1. Physical materials: table, vehicle tires and vehicle body contacts done (`docs/physics-materials.md`); character traction is done (`CapsuleInput::traction`, derived by the host from the surface under the feet: no change to what the controller reports); object restitution and friction are done as a reusable response (`contact_velocity`, not wired into Showcase 03); next is a Showcase world that builds a `SurfaceMap` from its surface enum (opt-in, since it changes the scripted drive).
2. Vehicle: tire load sensitivity and a handbrake are done (opt-in, `docs/physics-materials.md`). The powertrain (torque curve, automatic gearbox, rpm and gear published) is done too. Next: a differential, damage state, a controlled-drift test, engine braking.
3. Rigid-body stacking and joints; evaluate Jolt and Bullet (license, determinism, maintenance) behind a backend interface; owner approval to adopt.
4. Water (buoyancy, drag), destruction (fracture of authored meshes).

## Characters and animation (20%)

1. Done (`docs/animation-character.md`): humanoid map, BVH import and loop extraction, retargeting, locomotion state machine with stride-matched playback, ground and wall foot IK, capsule glue, real UAL and CMU assets.
2. Done: foot locking (`lock_planted_feet`): the retargeted capture cycles went from 10 to 27% to 0.1 to 4.8% of the speed. Lock, ground IK and the wall check are behind one call (`FootPlacement`). A pelvis correction for a lock that pulls a leg to its limit was measured and is not needed (worst miss 0.7 mm over the real cycles at 70 to 140% of their speed).
3. Replace CesiumMan in Showcase-class apps with the UAL rig and this controller (not Showcase 03: it stays as delivered); upload the UAL mesh through gfx_scene skinned staging.
4. Done: root-motion actions (`action.hpp`, `CapsuleInput::drive`; roll and attack verified on the UAL clips). Next: strafing and turn in place (the UAL has no clips for them: procedural or mocap); crouch transitions, crouch jump and slide (crouch itself is done: capsule `crouch` input and the UAL crouch set with `StanceBlend`); aligning the character to the ledge before a climb (the vertical action and `find_ledge` are done), vaulting, a climb with a hang (additive layers are done: `blend_additive`); an action heading change when a clip needs it; strafing and turn in place need an asset source (neither UAL nor UAL 2 has them; the CMU host is not reachable from this session's network); a vehicle-entry sequence that combines hand IK, an action and a door (hand IK `reach_hand`, upper-body layering and look-at are done); joint limits for the arm; a second jump.
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
