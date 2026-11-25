# Development state

Checkpoint: 2026-11-24. `main` is at 5239e0c (merge of #59, sorted alpha blending). Nothing below has been merged by this program; nothing has been validated on a physical GPU.

## Branches that carry the Showcase 03 and infrastructure work (no PR yet)

| Branch | Head | Contains | CI (Graphics workflow, Linux and Windows) |
| --- | --- | --- | --- |
| `claude/showcase-03-world` | 1d77156 | the film's world, simulation, render app, tools; the CI warning fix | passed after the warning fix |
| `claude/production-infrastructure` | the commit that carries this file (see `git log`) | everything in `showcase-03-world` plus the asset catalog and production tools, the three Showcase 03 defect fixes (D1 emissive gain, D2 wheel rig, D3 prop table) and these documents | 6a6893d (D1, D2, D3, audit, Windows scripts, physics surfaces): passed (run 579). Earlier heads: 895d0d8 passed; 44a5d37 was cancelled by a later push; cae1e23 failed once on Windows (a test target lacked C++20), fixed in 6a6893d |

The film was built from 9c87457 (ancestor of both branches). It is unchanged.

## Humanoid animation increment (on `claude/production-infrastructure`, no PR)

`engine/animation` (target `saige::animation`) with its tests (`saige_animation_tests` 27 cases, `saige_animation_character_tests` 6, `character_drive` (Core), `saige_animation_assets` skipped unless `SAIGE_ANIM_ASSETS` points at fetched assets), the runnable `saige_anim_character_demo`, `tools/animation/` (asset fetch with SHA-256 manifest), six CMU mocap catalog records, and `docs/animation-character.md`. Root-motion actions (`action.hpp`, `CapsuleInput::drive`, roll and attack verified on UAL clips) are in the same module. The glTF importer now carries `Skin::joint_names`. Local result: all pass under the CI flags on software Vulkan hosts (the module is CPU only). CI: run 583 on `75f888b` passed on all three jobs (module, foot locking and character traction). The one earlier red (run 582, Windows only) was a test comparing two identical poses with `pose_distance == 0`, which MSVC's `acos` rounding made 7e-4 rad; `rotation_angle_between` is now exact for identical rotations and uses atan2. Run 584 on `df60e1a` (root-motion actions) passed on all three jobs. Run 585 on `18efc53` (`contact_velocity`, object restitution and friction on surfaces) passed on all three jobs. Run 586 on `8aa4669` (upper-body layering and look-at, `layers.hpp`) passed on all three jobs. Run 587 on `6992aff` (hand IK `reach_hand`, crouch with `CapsuleInput::crouch`, the UAL crouch set and `StanceBlend`) passed on all three jobs. Run 588 on `0dda8c1` (vertical root-motion actions: scripted 3D `CapsuleDrive`, `ActionClip` vertical, UAL 2 climb) passed on all three jobs. Run 589 on `9842715` (`find_ledge`, `blend_additive`) passed on all three jobs. Run 591 on `021ece0` (`FootPlacement`, the foot-lock reach diagnostic, the vehicle handbrake and tire load sensitivity) passed on all three jobs. Nothing pending.

## Open pull requests (all draft except #64 and #67), grouped by stack

* Physics: #69 capsule-box sweep, #70 capsule character (on #69), #73 capsule-triangle sweep (on #69), #74 capsule on triangles (on #70, #73), #80 static collision (on #73), #88 oriented-box and wheel probes (on #80), #65 character collision course, #68 perf instrumentation.
* Render, on `main`: #71 scene lighting, #75 depth-only RHI, #77 cascade fitting, #78 constructor rollback, #81 glTF skins, #82 IBL precompute, #83 frustum culling, #92 bloom, #95 FXAA, #98 SSAO.
* Render, stacked: #76 sun shadows (on #75, #71, #77), #79 alpha-tested shadows and #89 cascades (on #76), #85 IBL GPU (on #76, #82), #87 GPU skinning (on #85, #81), #90 atmosphere (on #71), #91 dynamic lighting (on #90, #89), #96 night sky (on #90), #84 draw state cache (on #83), #94 prepared model (on #87), #99 animated bounds, #93 integration candidate (#91 + #87 + #83), #97 sky IBL (on #93).
* Core: #60 chunk staging, #63 chunk manifests (on #60), #86 live world origin migration, #64 saved-project transparency test, #42 single-pass project open.
* Platform: #44 Vulkan-Headers 1.4.363 build, #45 Vulkan version negotiation (on #44), #67 descriptor rollback (ready for review).
* Evidence and bookkeeping, not for merge: #72 Showcase 01, #34 0.2 integration, #54 0.3 PBR integration, #61 M5 and M6 preparation, #56 0.3 handoff, #58 MikkTSpace tangents.

Merge order and the base each PR must be re-reviewed against belong to the owner's decision per `AGENTS.md`; this program proposes (but does not perform) landing in this order: platform (#67, #44, #45), Core (#64, #60, #63, #86), physics (#69, #70, #73, #74, #80, #88), then the render stacks bottom up (#71, #75, #77, #76, then the rest), re-running the checks on each exact head against its proposed base.

## Test evidence at this checkpoint

Local, Debug with `-DSAIGE_GFX_WARNINGS_AS_ERRORS=ON`, software Vulkan (lavapipe, CPU only, 4 cores): `ctest` on `claude/production-infrastructure` plus the D1 change: 110 tests, 0 failed, 1 skipped
(`production_tools_python`: the tools venv is not at its default path in this container; run by hand with `SAIGE_TOOLS_HOME` set, its 10 tests pass). Includes `horizon_sim` (about 47 s), `horizon_props`, `horizon_wheel_rig`, `horizon_time_of_day`, the 151 Vulkan model, shadow,
sky and lighting tests, and the shader-header consistency check. The new tests were each made to fail against their defect (mutation or control) and restored.

Hardware class: software Vulkan only. No frame time in this repository's documents is a physical GPU result.

## Open items

* Owner decisions needed: none for this increment. Pending for later: adopting a physics library (Jolt or Bullet), adopting an audio runtime, any merge to `main`.
