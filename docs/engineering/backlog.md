# Backlog (dependency-ordered; top of each group first)

## Defects and debt

* Barrel stack prop is non-solid (multi-piece model); give it a compound hull.
* The unlit window material is a flat dark glass; lit windows have no interior and light nothing around them.
* Wheel-of-a-wagon aliasing at 30 fps; add temporal supersampling (average sub-frames) to the offline capture path before any further film.
* CI asset restoration (done; run 605 passed on all three jobs with `saige_animation_assets`, `animation_viewer_smoke` and `production_tools_python` running instead of skipping; only `horizon_props` still skips, it needs the Showcase 03 assets): the Graphics workflow installs the production tools' Python packages (so `production_tools_python` runs) and restores the humanoid assets with `tools/animation/fetch_humanoid_assets.py --no-cache` (download, archive and file SHA-256, retries; cached by the manifest hash and re-verified), so the real-asset animation tests and the viewer smoke test run in all three jobs instead of skipping. The CMU hashes come from raw.githubusercontent.com; the UAL archives from opengameart.org.

## Physics (22%)

1. Physical materials: table, vehicle tires and vehicle body contacts done (`docs/physics-materials.md`); character traction is done (`CapsuleInput::traction`, derived by the host from the surface under the feet: no change to what the controller reports); object restitution and friction are done as a reusable response (`contact_velocity`, not wired into Showcase 03); next is a Showcase world that builds a `SurfaceMap` from its surface enum (opt-in, since it changes the scripted drive).
2. Vehicle: done and opt-in (`docs/vehicle-dynamics.md`): brake pads and calipers stay on the hub in the Showcase 03 code (not re-rendered), differential, engine braking, slip-angle lateral tire with a controlled drift, persistent damage state with physical consequences and a versioned save format. Not done: tire slip-ratio and wheel-speed state, mesh deformation (not claimed), vehicle-vehicle contact. Earlier: tire load sensitivity and a handbrake are done (opt-in, `docs/physics-materials.md`). The powertrain (torque curve, automatic gearbox, rpm and gear published) is done too. Next: a differential, damage state, a controlled-drift test, engine braking.
3. Rigid-body stacking and joints: done behind `DynamicsWorld` with the optional Jolt v5.7.0 adapter (`docs/physics-backends.md`; gates pass on Linux and Windows). Next: two-way coupling of the character and the vehicle with dynamic bodies, vehicle-body collision with backend bodies, force application for buoyancy on dynamic bodies, performance workloads (CPU, not GPU), cross-platform determinism measured with `JPH_CROSS_PLATFORM_DETERMINISTIC`, the harness re-run on v5.7.0. Permanent adoption is the owner's.
4. Water: done as a module (`docs/water.md`: Gerstner surface, dispersion by depth, buoyancy, drag, currents, stability) and measured at 1 to 100 bodies (distance-based cells, deterministic threads). Next: a host that spawns wakes and foam from `FloatingBody::wetted()`, swimming, a depth map for coastal shoaling, a time-varying wind. Destruction (fracture of authored meshes) is not started.

## Characters and animation (20%)

1. Done (`docs/animation-character.md`): humanoid map, BVH import and loop extraction, retargeting, locomotion state machine with stride-matched playback, ground and wall foot IK, capsule glue, real UAL and CMU assets.
2. Done: foot locking (`lock_planted_feet`): the retargeted capture cycles went from 10 to 27% to 0.1 to 4.8% of the speed. Lock, ground IK and the wall check are behind one call (`FootPlacement`). A pelvis correction for a lock that pulls a leg to its limit was measured and is not needed (worst miss 0.7 mm over the real cycles at 70 to 140% of their speed).
3. Replace CesiumMan in Showcase-class apps with the UAL rig and this controller (not Showcase 03: it stays as delivered). Done for a standalone course: `saige_anim_viewer` stages the UAL mesh through gfx_scene skinned staging and drives it with the capsule and the whole animation chain (`docs/animation-character.md`); wiring it into a Showcase-class app is the remaining step.
4. Strafing and turn in place: the CMU host is still unreachable, but the same capture files are on the raw host the project already uses, and 1,223 of them were scanned for turn-in-place and sidestep root motion; eight candidates were recorded in `tools/animation/assets.json` (`cmu_candidates`) and then reviewed by eye with `tools/animation/bvh_contact_sheet.py`: **none is usable** (five of the six 'turns in place' were the T-pose first frame of every CMU file turning into the first real pose, a yaw change of about 90 degrees that is not in the motion; the others are a bow with arm swings, a diagonal walk and a turn while walking). The scan was corrected (it skips the T-pose frame) and re-run; its result is recorded below when it has been reviewed. Procedural alternatives (stepping the feet around a turn with the foot IK) are not built. 4b. Ledge alignment (`ledge_alignment`, `find_ledge_scan`: square to an oblique face and to the stand distance from any heading that sees it, none of 12 starts overshoots) is done. Arm joint limits and body avoidance for hand IK, and a vehicle-entry reference sequence (`seat_entry.hpp`): done on a synthetic humanoid (next: the real UAL rig, exit, door closing, stepping). Done: root-motion actions (`action.hpp`, `CapsuleInput::drive`; roll and attack verified on the UAL clips). Next: strafing and turn in place (the UAL has no clips for them: procedural or mocap); crouch transitions, crouch jump and slide (crouch itself is done: capsule `crouch` input and the UAL crouch set with `StanceBlend`); aligning the character to the ledge before a climb (the vertical action and `find_ledge` are done), vaulting, a climb with a hang (additive layers are done: `blend_additive`); an action heading change when a clip needs it; strafing and turn in place need an asset source (neither UAL nor UAL 2 has them; the CMU host is not reachable from this session's network); a vehicle-entry sequence that combines hand IK, an action and a door (hand IK `reach_hand`, upper-body layering and look-at are done); joint limits for the arm; a second jump.
5. Ragdoll and motor-driven animation: foundations done (`docs/ragdoll.md`: anatomical ragdoll, momentum-continuous start, recovery blend, joint motors, ankle strategy). Next: the UAL skeleton mapping and a real-rig transition test, get-up animations by `lying_face_up`, stepping recovery, hands, per-joint torque limits, impact-reactive behaviours (the Euphoria direction), vehicle entry and exit with the real rig, strafing and turn-in-place from the reviewed CMU candidates, smooth crouch transitions.
6. A speed ladder agreed with the animator: the UAL has a walk at 1.05 m/s and a sprint at 8.96 m/s and no jog; add a jog and check the capsule's walk and sprint speeds against the cycles.

## Core architecture (21%)

1. CPU baselines of the vehicle, character and water steps are recorded (`baselines.md`); the Showcase 03 replay under the perf instrumentation (#68) and memory baselines are not, and are needed before changing entity storage or threading.
2. Project and asset format versioning with migrations.

## Assets and tools (13%)

1. `catalog validate` in CI; license matrix check.
2. Windows bootstrap and qualification scripts.
3. Toolchain and dependency audit: done as `tools/audit` (offline; policy in `policy.json`). Next: run it in CI and on the Windows machine, and record the result per release candidate.

## World simulation and AI (10%), graphics (7%), audio (7%)

* Navigation over streamed cells and pedestrian agents: done as a lattice (`docs/navigation.md`). Next: a hierarchical level and cost fields (roads), off-mesh links, schedules, traffic.
* Quality presets; point lights; capability-gated RT and upscalers.
* Production audio runtime evaluated against the miniaudio proof of concept. The simulation side is done and backend-independent (`include/saige/audio_model.hpp`: engine loop pitches and equal-power crossfade from the published rpm and throttle with inertial smoothing; impact category, gain and pitch from the materials and the collision energy); choosing and integrating the runtime itself is not.

## Automation

* A bounded, single-writer, checkpointed scheduled engineering run (see `agent-handoff.md`). Not enabled until configured and verified.
