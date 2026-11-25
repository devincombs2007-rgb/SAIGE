# Capabilities (what exists, and how far it is proven)

Columns: **I** implemented on a branch, **T** tested locally, **C** CI-validated on the exact head, **M** merged to `main`, **H** validated on a physical GPU or device. "Software" means lavapipe on the CPU:
correctness evidence only, never performance evidence. No capability here is hardware-validated.

| Area | Capability | I | T | C | M | H | Where | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Rendering | HDR, PBR metallic-roughness, normal mapping, alpha blend | yes | yes | yes | yes | no | engine/render | blend is in main (#59) |
| Rendering | Sun shadows, three or four cascades, PCF | yes | yes | draft PRs | no | no | #75, #76, #89 | |
| Rendering | Atmosphere, sun disc, moon, stars, time-of-day lighting | yes | yes | draft PRs | no | no | #90, #96 | |
| Rendering | Per-frame lighting and exposure, ambient floor, **emissive gain** | yes | yes | #91 yes; emissive gain yes (run 579) | no | no | #91 | emissive gain is new |
| Rendering | IBL (precompute and GPU) | yes | yes | draft PRs | no | no | #82, #85 | not used by Showcase 03 (fixed per renderer) |
| Rendering | Bloom, FXAA, SSAO | yes | yes | draft PRs | no | no | #92, #95, #98 | |
| Rendering | GPU skinning, frustum culling, animated bounds | yes | yes | draft PRs | no | no | #87, #83, #99 | |
| Rendering | Point lights, ray tracing, DLSS, FSR, XeSS | no | no | no | no | no | - | not implemented; not operational |
| Physics | Capsule character on boxes and triangles | yes | yes | draft PRs | no | no | #69, #70, #73, #74 | |
| Physics | Physical surface materials (table, SurfaceMap) used by the vehicle (tires, body) and the character (traction) | yes | yes | yes (run 579) | no | no | `include/saige/physics_material.hpp`, `docs/physics-materials.md` | tire grip, rolling resistance, body-contact friction and restitution, character traction |
| Physics | Static collision (BVH, ray casts, region and shape queries), oriented-box and wheel probes | yes | yes | draft PRs | no | no | #80, #88 | |
| Physics | Raycast vehicle (suspension, tire friction circle, powertrain, oriented-box chassis, impacts) | yes | yes | yes | no | no | `include/saige/vehicle.hpp` | on the showcase branches |
| Physics | Vehicle options: differential (open, locked, limited slip), engine braking, slip-angle lateral tire (controlled drift), persistent damage state with a versioned save format | yes | yes | differential, engine braking and slip-angle tires passed (run 603); damage pending | no | no | `docs/vehicle-dynamics.md` | all opt-in and bit-identical when off; damage is a numeric state with physical consequences, not mesh deformation |
| Physics | Water: Gerstner wave surface (dispersion by depth, height, normal, flow, foam, wind sea states), floating bodies (buoyancy, drag, currents, stability) | yes | yes | no | no | no | `docs/water.md` | design hooks for wakes, swimming, coastal waves and weather are documented, not built |
| Physics | Dynamic rigid bodies behind `DynamicsWorld` (optional Jolt v5.7.0 adapter; constraints, motors, CCD, groups) | yes | yes | Linux single and double, Windows MSVC (Physics Jolt run 1) | no | no | `docs/physics-backends.md` | off by default; permanent adoption awaits the owner; GPU/workstation numbers none |
| Physics | Buoyancy and water drag for dynamics bodies | yes | yes | with the dynamics job | no | no | `docs/water.md` | boxes only |
| Characters | Skeleton to and from the ragdoll (`ragdoll_glue.hpp`) | yes (synthetic rig) | yes | skipped without Jolt in the Graphics job | no | no | `docs/ragdoll.md` | not run on the UAL rig |
| Characters | Humanoid ragdoll, recovery blend, joint motors, ankle balance | yes | yes | with the dynamics job | no | no | `docs/ragdoll.md` | not mapped to the UAL rig yet; no stepping or get-up |
| Core | Large-world positions (cell + local float, double arithmetic) | yes | yes | yes | no | no | `docs/large-world.md` | |
| Physics | Physics backend evaluation (Jolt v5.3.0 and Bullet 3.24 behind one interface) | yes (isolated prototype, not in the root build) | yes | docs and tools only (run 602) | no | no | `tools/physics_eval`, `docs/engineering/physics-backend-evaluation.md` | recommendation: Jolt behind a SAIGE-owned interface; adoption awaits owner review |
| Animation | glTF skins and clips, CPU reference, GPU skinning | yes | yes | draft PRs | no | no | #81, #87 | |
| Animation | `saige::animation`: humanoid map, BVH import, loop extraction, retargeting, two-bone and foot IK, locomotion state machine with stride-matched playback, capsule glue | yes | yes | run 600 passed (all of the above, plus foot placement, the vehicle powertrain and the viewer) | no | no | `engine/animation`, `docs/animation-character.md` | CPU only; real UAL and CMU clips; foot locking and root-motion actions (roll, attack) on the capsule; upper-body layering, look-at, hand IK, crouch (capsule input plus the UAL crouch set) and vertical root-motion actions (a 1 m climb on UAL 2), a ledge query and a renderer viewer (`saige_anim_viewer`); strafing and turn in place (no clips), ledge alignment, arm joint limits not yet |
| Animation | Hand IK arm constraints (elbow flexion range, shoulder envelope, body volumes) and a vehicle-entry state machine with IK pose (approach, reach the handle, open the door, step in, sit with hands on the wheel and feet on the pedals) | yes | yes | pending (this commit) | no | no | `engine/animation/include/saige/animation/ik.hpp`, `seat_entry.hpp` | synthetic humanoid tests; the feet slide into the footwell (no stepping), no door collision, no exit, no closing the door |
| Core | Streaming cells, origin migration, background preparation | yes | yes | draft PRs | no | no | #60, #63, #86 | |
| Core | Perf instrumentation | yes | yes | draft | no | no | #68 | |
| Showcase | Showcase 03 sim, props table, wheel rig, time-of-day ramp | yes | yes | yes (run 579) | no | no | `claude/production-infrastructure` | film delivered, build 9c87457 |
| Assets | Catalog (about 105 records), validator and importer check, prep pipeline | yes | yes | yes | no | no | `tools/production`, `assets/catalog` | one Python test skips unless the tools venv is at its default path (its 10 tests pass when run with `SAIGE_TOOLS_HOME` set) |
| World | Navigation lattice per chunk, streamed map, A* with string pulling, pedestrian agents with avoidance | yes | yes | pending | no | no | `include/saige/navigation.hpp`, `docs/navigation.md` | no navmesh, one floor per column, no off-mesh links |
| Audio | Engine rpm and load audio model, material impact sound selection (pure functions, no audio library) | yes | yes | pending | no | no | `include/saige/audio_model.hpp` | feeds any loop and one-shot player; no runtime chosen |
| Audio | miniaudio proof of concept (Doppler, pan, attenuation, occlusion, reverb, RPM blend) | yes | yes | no | no | no | `docs/assets/audio.md` | PoC, not an engine subsystem |
