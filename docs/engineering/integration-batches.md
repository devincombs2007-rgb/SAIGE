# Review batches (INTEG-3)

The 15 pull requests that did not merge in the first candidate (`integration-candidate.md`) were investigated **one by one**, resolved by hand where they conflicted with what lands before them, and the whole stack was cut into small,
dependency-ordered **review batches** that each build and pass on their own. Nothing was pushed to a remote branch other than `claude/production-infrastructure`, nothing was merged into `main`, no pull request was opened or changed:
the batches are reproduced locally from the pull request heads and the resolution patches in `tools/integration/batches/` (below), and the owner decides whether they become branches and pull requests, and in what order they land.

Base: `main` 5239e0c. Every pull request head is taken exactly as GitHub shows it (the SHA is in `tools/integration/batches/manifest.json`).

## The batches

{{BATCH_TABLE}}

Reading the table: each row builds on the one above it (a batch's tree contains all earlier batches). "Tests" is `ctest` of the whole tree at that tip: Debug, Ninja, GCC 13, **`-DSAIGE_GFX_WARNINGS_AS_ERRORS=ON`**, software Vulkan
(Mesa lavapipe, CPU only) under Xvfb, Vulkan-Headers 1.4.363 (the pinned commit, fetched through the raw host because `github.com` is not reachable from the cloud session). **No GPU result is claimed**; Windows and the hosted CI
have not seen the combined batches (they need a branch on GitHub), only each pull request alone.

## The 15 conflicts, one by one

| PR | Why it did not merge | What it needed | Resolution (evidence in the batch's tests) |
| --- | --- | --- | --- |
| #92 bloom | `SceneRenderer` was restructured by #78 (`acquire()`/`release()`, validated settings in the member initialiser) after #92 was written | validate bloom *and* lighting in the initialiser; free the bloom chain in `release()` (it is `noexcept` and a no-op for a chain never built) | both validations chained; chain freed in `release()`. `Bloom (null device)` tests, 82 -> 85 render cases |
| #95 FXAA | the same constructor and destructor, and the post-process pass chain | an intermediate target for the tone-mapped image, `release_targets` taking it, the bloom chain and the FXAA input created together | `release_targets(device, depth, hdr, hdr_input, aa, aa_input, ao)`; tone map writes the intermediate, FXAA reads it. FXAA tests pass; see "composition" below |
| #98 SSAO | 12 hunks: the same functions again, plus the order of the passes | occlusion must run **before** bloom (bloom must bloom what the tone map shows), and bloom's chain must read the occluded colour | AoGenerate/AoApply first, bloom and tone map read `tonemap_input` (the occluded colour), the chain is created from it. New test `Post chain (null device)`: bloom + occlusion + FXAA together add exactly what each adds alone (draw and pass counts over two frames, no leak, no validation error) |
| #76 sun shadows | `ModelRenderer` had been given the same `acquire()`/`release()` by #78; both sides also declared them in the header | take #76's (a superset: shadow validation, ring of constants groups) | textual merge left **duplicate declarations and a duplicate test helper**: found by building, not by merging, and fixed |
| #79 alpha-tested casters | none once #76 is in (it merges cleanly) | - | builds and passes on its own base |
| #89 cascades | one hunk each in `scene_renderer.cpp` and `shadows.md` | keep the frustum-culling and cascade settings together | resolved; docs say "blended surfaces do not cast; alpha-masked ones do" |
| #85 IBL GPU | settings validation and generated shader headers | validate the environment with the shadows, regenerate the shader headers | `saige_regenerate_shaders`: the IBL permutations changed because the merged `model.hlsl` now combines shadow cascades and IBL; `shader_up_to_date` checks pass |
| #87 GPU skinning | the shadow draw loop: #79 chose its pipeline per primitive (opaque or alpha-tested), #87 added a skinned caster with palette bind groups; **the textual merge interleaved both into code that compiled and was wrong** | one decision per primitive: skinned caster, alpha-tested caster, or opaque caster; palette set 0 vs material set 0 | rewritten as one `pipeline` choice with `bound_palette` reset on every pipeline change. **Known gap, documented:** no skinned alpha-tested caster exists (neither PR wrote one), so a skinned primitive whose material is alpha-tested casts as if opaque. Backlog item `ENV-GAP-1`; nothing was lost |
| #90 atmosphere | a third restructuring of the same constructor and destructor | validate the atmosphere with the others; sky handles freed in `release()` | resolved; atmosphere and sky tests pass |
| #91 dynamic lighting | the constants **ring** of #91 (one buffer per frame in flight) overlaps the shadow ring of #76 and the IBL bindings of #85 | one builder for the constants bind group, taking the scene buffer as an argument | `constant_bindings_for(shadow_block, scene_buffer)` builds shadow, IBL and dynamic variants; the dynamic non-shadow case reuses it. (A first resolution left conflict markers in the file: caught by the build.) |
| #94 prepared model | #94 splits `stage_model.cpp` into `prepare_model` (CPU) and `upload_model`, so every later change to the old function has to move | cherry-picked on the stack; #79's shadow-casting rule moved into `prepared_model.cpp` | a **semantic** seam, found by `test_stage_model` (it asserts MASK materials cast): the rule is carried over |
| #99 animated bounds | based on #93, which is a combination, not a feature; and #94 moved the staging code | cherry-picked; the per-joint bounds are computed in `prepare_model` | `joint_bounds` in `PreparedPrimitive`; random-skeleton containment tests and the Vulkan sweep pass |
| #93 integration candidate | it *is* a combination of #91, #87 and #83 | its one own commit matters: **never frustum-cull a skinned primitive by its bind-pose bounds** (without it #83 + #87 cull animated characters by their rest pose) | that commit is applied; the rest of #93 is the same content as the batches. #97 and #99 are applied on top |
| #97 sky environment | based on #93 | its two own commits | cherry-picked; `test_living_sky` added to the Vulkan tests |
| #74 character on triangles | criss-cross history between #70 and #73 | the root `CMakeLists.txt` test list | union of the lists; builds and passes (follow-up on the whole stack) |
| #86 live world origin | based on `core/playable-world-03` (81 commits that are not on `main`); `src/project.cpp` includes | `project_load.hpp` (#42) and `atomic_save.hpp` (#86) | both includes kept; builds and passes (follow-up on the whole stack, since it needs that base) |

Two findings are worth the owner's attention beyond the resolutions:

* **A textual merge can compile and be wrong.** #87 + #79 (the shadow loop), #94 + #79 (a rule moved to a new file) and #83 + #87 (culling skinned meshes by their rest pose) all merged or nearly merged without a single marker.
  Each was found by building, running the tests, or reading the seam. The seam tests are in the batches (`Frustum culling and skinning`, `test_stage_model`, the compose test). A merge of these stacks is not mechanical, and the owner should not be asked to
  trust a green merge button.
* **`tools/integration/merge_candidate.py` is a first pass, not a landing tool.** Its additive resolver resolved the root CMake lists correctly and the `.cpp` hunks only when both sides really only added lines. It now has `STOP_AT_HUMAN=1`
  to stop on the first hunk that needs a person, and `ORDER="..."` to run a chosen sequence.

## Reproducing the batches (no branch is pushed)

`tools/integration/batches/manifest.json` lists the 43 steps (fast-forward to #67, then a merge, a patch or a cherry-pick per pull request head, with the head SHA and the SHA of the tree that was validated); `patches/` holds the
**resolution** of each conflicted merge (the difference between what `git merge-tree` produces on its own, with normalised diff3 markers, and the validated tree) and the ported or fixed commits. Then:

    python3 tools/integration/batches.py verify          # replays all steps from the pull request heads; every tree must equal the recorded one
    python3 tools/integration/batches.py rebuild         # creates local branches batch/1-platform ... (nothing is pushed)

`verify` ran here and passed for all steps. The pull request heads are fetched with `git fetch origin refs/pull/N/head`.

## Batches 6 and 7 (what the program built itself)

{{PRODUCTION_SECTION}}

## What was not done

* No branch was pushed and no pull request opened or changed (the standing rule: a PR only when asked, no branch other than the working one without permission). Say the word and the batches become branches `batch/*` with a draft PR each, each describing the observable behaviour and linking issue #2 or its successor.
* Windows and hosted-CI evidence for the *combined* batches (they need branches on GitHub). Each pull request alone has its own CI.
* A physical GPU run (none exists in this environment). Image tests on lavapipe are software results.
* Core's pull requests (#42, #60, #63, #64, #86) were not edited beyond the two include lines of #86; they are Core's (`AGENTS.md`).
* #68 (perf instrumentation) is validated as a follow-up on the stack, not inside batch 2: it adds a root `CMakeLists.txt` test line like the others.
* Showcase 04 and the remake of Showcase 03 stay unauthorised and untouched.
