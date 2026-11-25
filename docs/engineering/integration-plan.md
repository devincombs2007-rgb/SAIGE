# Integration plan (proposal; nothing here has been merged by this program)

Checked 2026-11-25 against the 45 open pull requests (all draft except #64 and #67) and `main` at 5239e0c. The order is dependency order: a stack lands bottom to top, each PR re-reviewed against the base it will actually have then (`AGENTS.md`).
Landing is the owner's decision; this document proposes, it does not act.

## Stacks (base to tip)

| Stack | PRs, bottom to top | Notes |
| --- | --- | --- |
| Platform | #67 (descriptor rollback, ready) , #44 (Vulkan-Headers 1.4.363, build only), #45 (version negotiation and capability probe, on #44) | no runtime change: the baseline stays Vulkan 1.3 |
| Core | #64 (saved-project transparency test, ready), #42 (single-pass project open), #60 (chunk staging), #63 (chunk manifests, on #60), #86 (live world origin, based on `core/playable-world-03`, not on `main`), #65 (character course, on #70) | #86's base branch is not on the `main` chain: re-base or land `core/playable-world-03` first |
| Physics | #69 (capsule-box sweep), then #70 (capsule character), #73 (capsule-triangle), #74 (character on triangles, on #70 and #73), #80 (static collision, on #73), #88 (oriented box and wheel probes, on #80) | the vehicle, water and the animation capsule glue in `claude/production-infrastructure` are written against these |
| Render, from `main` | #71 (lighting), #75 (depth-only RHI), #77 (cascade fitting), #78 (constructor rollback), #81 (glTF skins), #82 (IBL precompute), #83 (frustum culling), #92 (bloom), #95 (FXAA), #98 (SSAO) | independent of each other |
| Render, stacked | #76 (sun shadows, on #75), #79 and #89 (on #76), #85 (IBL GPU, on #76), #87 (GPU skinning, on #85), #90 (atmosphere, on #71), #91 (dynamic lighting, on #90), #96 (night sky, on #90), #84 (draw state cache, on #83), #94 (prepared model, on #87), #93 (integration candidate, on #91), #97 and #99 (on #93) | #93 is a validated combination, not a PR to merge |
| Evidence, not for merge | #72, #34, #54, #61, #56, #58 | |

## Proposed landing order

1. Platform: #67, #44, #45. 2. Core: #64, #42, #60, #63, #86 (after its base), then #65 with its physics base. 3. Physics bottom up. 4. Render from `main`, then the stacks bottom up (#75, #76, #79, #89, #85, #87, #94; #71, #90, #91, #96).
`claude/production-infrastructure` (assets and tools, Showcase 03, the animation module, the vehicle, water and the evaluation) is the integration branch for this program's own work; it would land after the physics and render stacks it depends on, or be split by directory.

## Vulkan 1.4 migration review (#44, #45)

* #44 (head fad8558eb, Graphics workflow run passed on the PR): build-time only. Installed headers are used only if a translation unit including that exact directory compiles with the 1.4 API (a patch number alone is not trusted; three CTest cases cover old, patch-only and good header sets); otherwise Vulkan-Headers `vulkan-sdk-1.4.363.0` is fetched by its full commit
  (6802bb47...), marked as system includes. It found that the Linux lane had been building against distro 1.3.275 headers while Windows used 1.3.296. Runtime requirement unchanged (1.3). Review notes: the fetch is a full clone, not shallow (a commit pin cannot be shallow-fetched by tag), which costs CI time but not reproducibility; offline builds use
  `FETCHCONTENT_SOURCE_DIR_SAIGE_VULKAN_HEADERS`; the validation layer is still the distro package (documented as a separate decision). No objection.
* #45 (head 7002dfc0f, run 358 passed): pure negotiation rules (instance = min(loader, requested cap, implemented); device = min(instance, device); a capability is core only with the version and the feature bit, an extension only with the extension and the bit) with unit tests, and a read-only probe; nothing is enabled and 1.3 stays the default. The core-1.4 branch needs a 1.4 loader: only the
  Windows lane has one, and there only llvmpipe-class software devices exist in CI, so the first physical-device evidence is still owed (hardware class: none yet). No objection; the probe's report should be kept in the CI log for each release candidate.

## What this pass did not do

* No baseline of CPU, memory or simulation cost was recorded (the `claude/perf-instrumentation` work, #68, is not merged here); `docs/engineering/backlog.md` keeps it as the first Core item, to be done before any change to entity storage or threading.
* No project or asset format versioning change. The two versioned formats added by this program (the vehicle damage record, the asset catalog) carry explicit versions and tests (`docs/vehicle-dynamics.md`).
* Windows bootstrap scripts (`tools/windows`) were not run on Windows: still the owner's machine.
* No re-review of the draft PRs other than #44 and #45; their head SHAs and bases above are as listed by GitHub today.
