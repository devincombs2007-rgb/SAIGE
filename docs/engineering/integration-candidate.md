# Integration candidate (INTEG-2)

A consolidated candidate built from the open pull requests, to learn what lands together and what does not. **Nothing was pushed or merged**: a throwaway worktree of `main` (5239e0c), the heads of 38 open PRs fetched by
`refs/pull/N/head` (2026-11-25), merged in the dependency order of `integration-plan.md` by `tools/integration/merge_candidate.py`, which resolves only conflicts where both sides merely added lines at the same place
or appended names to one list line, and reports everything else. Landing, and the base each PR is re-reviewed against, remain the owner's decision (`AGENTS.md`).

## Result

| Outcome | PRs |
| --- | --- |
| Merged cleanly (14) | #67, #44, #64, #42, #60, #63, #69, #70, #65, #71, #77, #81, #96, #84 |
| Merged with an additive resolution (9; the file list says what was joined) | #45 (rhi_vulkan CMake list), #73, #80, #88 (root CMake test lists), #75 (CMake list; an include in vulkan_device.cpp), #78 (model_renderer.cpp, test_scene_renderer.cpp), #82 (gfx_assets CMake list), #83 (test_scene_renderer.cpp, rhi_vulkan CMake list), #90 (scene_renderer.cpp, rhi_vulkan CMake list) |
| Needs a human (15) | #86 (src/project.cpp), #74 (root CMake: a criss-cross merge), #92, #95, #98 (scene_renderer.cpp), #76, #79, #89, #85, #91 (model_renderer.cpp), #87, #94, #99, #93, #97 (model_renderer.cpp, gfx_assets CMake) |

The 23 PRs that merged **build and pass**: Debug, Ninja, GCC, software Vulkan (lavapipe, CPU only): 58 of 58 CTest cases, 371 commits over `main`. That says the stacks below the conflicts (platform, Core, physics except #74, the first
render PRs) coexist; it says nothing about the 15 that did not merge and nothing about GPU hardware. The additive resolutions in code files (#75, #78, #83, #90) compile and pass but are a person's call to confirm: both sides'
lines were kept in the order ours-then-theirs.

## Where the conflicts are

* `engine/render/src/model_renderer.cpp` and `scene_renderer.cpp`: touched by almost every render PR (#76, #79, #89, #85, #87, #91, #92, #94, #95, #98, #99, #93, #97). These do not combine by merging in parallel: each needs re-stacking on the one
  before it, in landing order, which is what the stack structure says and what the proposed order does. The independent "from `main`" PRs #92, #95, #98 (bloom, FXAA, SSAO) all edit `scene_renderer.cpp`: only one of them can be first and the other two rebase on it.
* CMake source and test lists (root, `rhi_vulkan`, `gfx_assets`, `shaders`): every PR appends a name to the same lines; mechanical, resolved by the script, and the reason a human merge of many PRs is tedious.
* `src/project.cpp` (#86, live world origin, based on `core/playable-world-03` rather than `main`) and #74 (criss-cross history between #70 and #73): need their authors' attention.

## Proposed use

Land bottom up as in `integration-plan.md`, re-running the checks on each exact head against its real base, and rebase the render stacks one at a time (the first render conflict in this run is #76 against the earlier render merges in `model_renderer.cpp`). The script
reproduces this evidence on any later `main` in a few minutes. Not done: resolving the 15 by hand (it is a day of careful review, not mechanical), running the Vulkan tests on a physical GPU or on Windows for the candidate, and a candidate that
includes this program's own branch (`claude/production-infrastructure`), which depends on the physics and render stacks above.

## Memory and workload profiling (this machine, CPU only)

Peak resident memory of every workload run so far is under 13.1 MB (`saige_sim_baseline` 78 s, `navigation_test crowd` with 400 agents 16 s, `dynamics_test`, `ragdoll_test`, `saige_water_workloads --cost-only` 230 s: the maximum over all of them
is 13.1 MB). Time baselines are in `baselines.md`; cost scaling of water bodies and crowds is in `docs/water.md` and `docs/navigation.md`. Not done: allocation counts per frame, a resource-lifetime report for GPU objects (the render PRs
own that: #67, descriptor rollback), streaming budgets in bytes and milliseconds per chunk, and any of this on the target workstation (the owner's machine, `tools/windows`).

## Format versioning (status)

Versioned with tests: the project and chunk manifest formats (Core), the vehicle damage record (`SVDM` v1, FNV-1a checksum, `docs/vehicle-dynamics.md`), the asset catalog. Not versioned yet: the `WorldPosition` in saved positions (cell + local float:
documented in `docs/large-world.md`, no file format carries it besides `CourseCheckpoint`, which is already cell + local), ragdoll or navigation data (runtime only). A migration policy (explicit version, a tested migration for each change, a refusal of a newer file) is
the rule in `master-directive.md`; each new persistent format adopts it when it is first written to disk.
