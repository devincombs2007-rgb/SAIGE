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
| Physics | Raycast vehicle (suspension, tire friction circle, drivetrain, oriented-box chassis, impacts) | yes | yes | yes | no | no | `include/saige/vehicle.hpp` | on the showcase branches |
| Animation | glTF skins and clips, CPU reference, GPU skinning | yes | yes | draft PRs | no | no | #81, #87 | |
| Animation | `saige::animation`: humanoid map, BVH import, loop extraction, retargeting, two-bone and foot IK, locomotion state machine with stride-matched playback, capsule glue | yes | yes | run 585 passed (through object contact response); layering and look-at pending | no | no | `engine/animation`, `docs/animation-character.md` | CPU only; real UAL and CMU clips; foot locking and root-motion actions (roll, attack) on the capsule; upper-body layering and look-at; strafing, turn in place, crouch, climbing, hand IK not yet |
| Core | Streaming cells, origin migration, background preparation | yes | yes | draft PRs | no | no | #60, #63, #86 | |
| Core | Perf instrumentation | yes | yes | draft | no | no | #68 | |
| Showcase | Showcase 03 sim, props table, wheel rig, time-of-day ramp | yes | yes | yes (run 579) | no | no | `claude/production-infrastructure` | film delivered, build 9c87457 |
| Assets | Catalog (about 105 records), validator and importer check, prep pipeline | yes | yes | yes | no | no | `tools/production`, `assets/catalog` | one Python test skips unless the tools venv is at its default path (its 10 tests pass when run with `SAIGE_TOOLS_HOME` set) |
| Audio | miniaudio proof of concept (Doppler, pan, attenuation, occlusion, reverb, RPM blend) | yes | yes | no | no | no | `docs/assets/audio.md` | PoC, not an engine subsystem |
