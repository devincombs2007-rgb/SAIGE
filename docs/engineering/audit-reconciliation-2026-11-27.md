# Audit reconciliation (first pass, 2026-11-27) — INCOMPLETE

Requested by the director comment 7207163196 on #43. This document reconciles the director's backlog priorities against `docs/engineering/backlog.md`, the Showcase 04 defect register (`docs/showcase/04-defect-register.md` on `claude/s04-quality` at `5d90353885ab40fc1bd7c6f3e8aea4d64256bcda`; it is not on this branch) and the repository's branches, PRs, issues and skipped tests.

**It is not a completed audit.** Section 5 lists what is not yet accounted for. Nothing here is a claim about physical-GPU behavior or performance: every rendering result in this repository was produced on software Vulkan (llvmpipe/lavapipe) or on the CPU.

Written at `claude/fidelity-dev` head `e0b08eab3f3e0f5cfffc48b7c7d98c74a54f289a`. `main` is `5239e0c16c33d408f99aa7ec00f4271742c09cb3`.

## 1. Vocabulary

| Status | Meaning |
| --- | --- |
| **verified** | Implemented, with a test whose hosted CI result I have seen at the cited SHA or run. |
| **partial** | Part exists and is tested; the rest is named. |
| **unintegrated** | Exists and passes its own tests on a branch or draft PR that is not on `main` and is not consumed by the Showcase 04 code. |
| **defective** | A defect is confirmed (reproduced or shown by data) and not fixed. |
| **missing** | Nothing exists beyond a document or a plan. |
| **unknown** | I have not checked, or the check did not decide it. |

Evidence levels: **CI** (hosted run id), **local** (my container only), **doc** (a repository document says so; not re-checked by me today), **none**.

## 2. Reconciliation by priority

### P0

| Item | Status | Evidence and paths | Unresolved criteria |
| --- | --- | --- | --- |
| Unified integration baseline (no main merge) | **unintegrated** | `main` is `5239e0c`. Seven dependency-ordered draft PRs exist (`#100`–`#113`, branches `batch/1-platform` … `batch/7-engine-systems`; docs on this branch: `docs/engineering/integration-plan.md`, `integration-batches.md`, `integration-candidate.md`; their content was not re-read for this pass). 59 PRs are open, 57 of them drafts; `claude/fidelity-dev`, `claude/s04-quality`, `claude/ssr-validation` are in none of the batches. | One reviewed baseline that contains the batches and the three active branches; its own CI; owner approval. Not started. |
| Test, skip and mutation integrity | **partial** | Lavapipe lane, run 45927354250: 175 tests, 169 passed, 6 skipped (`dynamics`, `destruction`, `buoyancy`, `ragdoll`, `saige_animation_ragdoll`: `CMakeLists.txt:132-144` and `engine/animation/CMakeLists.txt:76`, exit code 77, Jolt-dependent; `horizon_props`: `showcase/CMakeLists.txt:52`, backlog says it needs the Showcase 03 assets). S4-039 (visual regression could skip silently) is fixed on `claude/s04-quality`; hosted result seen at the time. Mutation counts are per feature (local lights 10/10, SSR 17 harness and shader mutants, light culling 14/14, host light set 12/12, CPU light reference 18/19 with one equivalent survivor). | Skips on the Windows and ASan lanes are not enumerated here. No repository-wide rule that a skip count may not grow. Mutation coverage of the older modules is not inventoried. |
| Project migrations and failure recovery | **partial / unknown** | Backlog (Core, item 2): "Project and asset format versioning with migrations" is listed as open. PR #64 (non-draft, saved v1 project transparency test) is open. `docs/project-format-0.2.md` exists. | Whether migrations exist and are tested on `main` or in the batches: not checked. |
| Physical-GPU evidence | **missing** | Issue #9 (Windows desktop host) and the `0.3` plan record the requirement. No result in this repository comes from a physical GPU. | A recorded run on physical hardware with adapter classification; owner action. |
| Lane successor, turn and crosswalk connectivity; all 33 curb ramps with shared collision, render and navigation | **defective** | S4-011 (partly fixed: validator rules for lane paths off the roadway), S4-031 (confirmed by data: every one of the 33 crosswalks ends at a raised curb). | A ramp that exists in collision, render and navigation for each of the 33; lane successor and turn connectivity rules with a failing-then-passing test. Not done. |
| Stair headroom, railings, reachability | **partial** | S4-012: landings kept clear; headroom, railings and upper-floor reachability not verified. | Validator rules and a test per criterion. |
| Mesh winding and overlap | **partial / unknown** | QC-2 (authoring validators) is in progress; winding is not checked anywhere I can point to. | A winding rule and an overlap rule with tests on the generated district. |
| Black corner props | **defective (cause unknown)** | S4-008: confirmed visible in frames 690 and 990; not identified (kiosk, bus stop or missing mate). | Identify the mesh, fix, add a validator. |
| Plinth with face mask 0 | **defective (latent)** | S4-020: the facade plinth box is created with face mask 0 and never draws (`build_building`). | Draw it, with a test. |
| Unresolved pop-event ranges | **unknown** | S4-009: popping not reproduced; thin-shadow aliasing confirmed. | A pop detector over the recorded frames with the ranges listed. |
| Camera composition and forced cuts | **partial** | S4-035 to S4-038: scout schema unified and strictly validated (DIR-1 done); seed 34 camera reproduction and cumulative displacement cap (DIR-2) in progress. | DIR-2 completion with a cross-seed test; DIR-3 fixture visual CI. |
| Asset provenance, axis and attachment verification | **partial** | S4-040 (chair fronts: validator rule, four-front test, rendered before/after, fixed at `5d90353`), S4-041 (hazard lamps: anchored on mesh surface vertices, rendered, tests). Sofa front axis could not be determined (documented, untouched). Other props (counter, shelf, display case, stool) are unchecked. Hazard lamps are added cubes, not the car's own light meshes (tracked separately). Asset catalog tooling exists (`tools/`, backlog: `catalog validate` in CI is still "next"). | Front-axis rule for every prop class; indicator-mesh integration; `catalog validate` in CI. |
| SSR findings A1–A4 (Gemini) | **open (Gemini)** | `claude/ssr-validation` at `61c37b42c4cac7b12b7a1e07588c0847d2784c54`: 20 executed tests, 17 mutation detections, required-mode CI. Production SSR blocked. | Gemini's algorithm changes and a real-render gate; Windows and ASan lanes have no SSR coverage. |

### P1

| Item | Status | Evidence and paths | Unresolved criteria |
| --- | --- | --- | --- |
| Local lights, forward | **verified (software Vulkan)** | `claude/fidelity-dev` `e0b08ea`; Graphics run 45927354250; `docs/rendering/local-lights.md`; `docs/validation/2026-11-27-model-local-lights/`. | No culling, no shadows, no combination with shadows, IBL, skinning or instancing, no GPU result. |
| Local lights, clustered | **unintegrated** | Host light set and clustered culling CPU reference and compute pass with tests at `d93fa14` (Graphics run 45854515120, success). Not wired into `ModelRenderer`. | Wiring behind the capacity setting with the forward result as oracle; no performance claim without a GPU. |
| Local light shadows and consumers (windows, lamps, headlights) | **missing** | S4-015 (no local illumination in the film), S4-032 (flat interiors). | Shadowed spot lights; the Showcase consumers (a new film is not authorized). |
| FrameView with previous transforms and palettes, velocity, jitter, history, TAA, exposure | **partial** | TAA CPU reference and GI evaluation documents (FID-2). No GPU TAA, velocity buffer or history. Exposure: S4-025 suspected (hand-written function of sun elevation). | Everything GPU-side; an exposure model with a test. |
| Physically based glass, IBL, scene reflections, interior mapping, detail mapping | **partial / unintegrated** | IBL GPU slice exists in the unmerged PRs #85 and #106; blue windows and reflections are owner-reported (S4-010) and not reproduced as defects beyond the register's confirmation of noisy reflections and opaque windshields. | Glass model; reflection source; interior mapping; detail maps. All missing. |
| Instancing and high-quality vegetation with wind, translucency, LOD | **partial** | Renderer instancing and GPU culling done (`docs/rendering/instancing.md`, `docs/rendering/gpu-culling.md`; RHI-4/5). `saige::environment` has weather, wind, vegetation scatter and wake as CPU modules with tests, not rendered (S4-029). | Vegetation shading, wind in the vertex stage, translucency, LOD. |
| GI probes and temporal GI | **missing** | Architecture evaluation document only. | Everything. |
| Vehicle-body coupling, wheel slip and visual sync | **unknown** | S4-014 suspected, not measured; `docs/vehicle-dynamics.md` describes the model. | A scripted drive with measured wheel and body sync. |
| Progressive damage and breakables | **partial** | S4-030: crumple is visual only. ENV-1 (breakables and persistent damage) in progress. | Progressive deformation tied to physics. |
| Rig foot placement, turns, entry, exit, active ragdoll | **partial** | `FootPlacement`, locomotion state machine, ragdoll foundations with tests on the synthetic rig and real UAL assets (`docs/animation-character.md`, `docs/ragdoll.md`). S4-013 (foot sliding, turning, curb and stair traversal in the film) not measured. CHAR-1/2/3 open. | Measurements on the film rig; turn-in-place and strafe sources (CMU host unreachable); vehicle entry and exit. |
| Hierarchical navigation, incident response | **partial** | Navigation lattice, multi-level nav, off-mesh links, dynamic obstacles, crowd stress, routines and traffic foundations are reported done (NAV-2); `backlog.md` still says "Next: a hierarchical level and cost fields", which is out of date relative to NAV-2. S4-017: emergency interaction not visible (22 m "on scene", 25 s clearing). | Reconcile the backlog text; activity-driven incident response. |
| Runtime spatial audio | **partial** | Miniaudio proof of concept isolated (AUD-1); simulation side backend-independent (`include/saige/audio_model.hpp`); AUD-2 (SAIGE audio interface over miniaudio) in progress. S4-021: audio measured, not listened to. | Runtime playback in the engine; listening review by the owner. |

### P2 and P3

| Item | Status | Evidence | Unresolved |
| --- | --- | --- | --- |
| Weather, wetness, particles, volumetrics | **partial / missing** | Weather CPU modules; smoke is stacked discs (S4-018); no wetness, volumetrics. | GPU consumers. |
| Water and shoreline | **partial** | Water module with buoyancy, drag, currents and optimization workloads (`docs/water.md`); boats are decorative, no wakes (S4-028); shoreline confirmed poor (S4-026). WATER-3 pending. | Host that spawns wakes and foam; shoreline blending. |
| Advanced materials; varied licensed asset kits | **missing / partial** | S4-019 and S4-027 confirmed (palette of 6, one window kit, repetitive buildings). Asset library architecture and prep pipeline exist. | A varied, licensed kit with provenance. |
| Texture and geometry residency, uploads | **partial** | `saige_residency` module with tests; streaming budgets (INTEG-2). Real texture streaming and upload are not done. | Measured residency against a budget on a real workload. |
| GPU-driven draws | **partial** | GPU culling checked against the CPU reference; indirect draws. | Draw submission from GPU-built lists in the renderer. |
| Density scaling | **unknown** | S4-016: 60 residents and about 14 vehicles; cost of raising it not measured. | 2x/4x measurement. |
| RT capability gates | **missing** | Capability bits exist for compute and indirect count; no ray-tracing gate. | Probe and fallback design. |

## 3. Reconciling the two documents

* `docs/engineering/backlog.md` (47 lines) has drifted from reality in at least these places: hierarchical navigation (listed as "next" but NAV-2 reports multi-level nav done); "Quality presets; point lights" (point lights now exist, see local lights); no entry for the S4 owner-reported items (S4-040, S4-041), SSR validation, local lights, light culling, or the audit priorities above. Updating it is a follow-up, not done in this pass.
* The defect register is the authority for Showcase 04 defects; its entries S4-001 to S4-041 are referenced above where they decide a status. Entries I did not map (for example S4-001 to S4-007, S4-022 to S4-024) were not re-read for this pass.

## 4. Inventory of the repository (counts are from this pass; raw data is committed beside this file)

| Item | Count | File |
| --- | --- | --- |
| Remote branches (`git ls-remote --heads`) | 142 (85 `claude/*`, 41 `core/*`, 14 `batch/*`, 1 `release/*`, `main`) | `audit/branch-inventory-2026-11-27.tsv` |
| of which every commit is already in `main` | 48 (besides `main` itself) | same |
| of which carry commits that are not in `main` | 93 | same |
| Last commit before 2026-11-24 (includes `main`, dated 2026-11-23) | 89 | same |
| Open pull requests | 59 (57 draft, 2 ready: #64, #67) | `audit/open-prs-2026-11-27.tsv` |
| Closed pull requests | 42, all merged | `audit/closed-prs-2026-11-27.tsv` |
| Open issues | 12 (#1, #2, #3, #4, #5, #7, #9, #17, #19, #43, #62, #66) | `audit/open-issues-2026-11-27.tsv` |
| Closed issues | 0 | |

Reproduce: `git fetch origin && git ls-remote --heads origin`; per branch `git rev-list --left-right --count origin/main...origin/<branch>`; PRs and issues with `gh api -X GET search/issues -f q='repo:devincombs2007-rgb/SAIGE is:pr is:open'` (and `is:closed`, `is:issue`).

## 5. Not yet accounted for (so this audit cannot be called complete)

1. **Per-branch content.** I counted the 93 branches with unmerged commits and recorded their distance from `main`. I did not classify each one (superseded by a batch PR, abandoned, live, or containing unique work). `claude/production-infrastructure`, `claude/ssr-validation`, `claude/fidelity-dev`, `claude/s04-quality` and `claude/showcase-04` are the furthest ahead (about 200 to 240 commits).
2. **Per-PR state.** CI status, base branch, overlap between a draft PR and the batch PR that contains it, and review state of the 59 open PRs.
3. **Issues.** Only titles of the 12 open issues were read for this pass; their acceptance criteria were not reconciled against the code.
4. **Source modules.** No module-by-module inventory (each directory under `engine/`, `include/saige/`, `src/`, `tools/`, `showcase/`) with its test and its last CI result.
5. **Skips on the other lanes.** Windows and ASan skip lists, and the Physics Jolt workflow's skip list.
6. **Mutation coverage** of modules older than this session's work.
7. **Defect register rows** not mapped above.
8. **Assertions in this table marked "doc"**, which I did not re-check today.

## 6. Proposed order for closing the gaps (for director review)

1. Classify the 93 unmerged branches and the 59 PRs against the seven batch PRs (a script plus a human-readable table); this decides the integration baseline.
2. Enumerate skips on all lanes and add a CI assertion that the skip set equals an allowed list.
3. Update `backlog.md` from this document.
4. Then return to P1 engineering in dependency order (clustered local lights, FrameView and velocity, then glass and reflections).
