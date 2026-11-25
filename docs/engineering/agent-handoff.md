# Agent handoff

For the next agent (Claude Code, Codex or a person) picking this up cold.

1. Read `AGENTS.md` (governance), `docs/architecture.md`, `development-state.md` (what exists where), `master-directive.md` (standing rules), and the issue that records your assignment (issue #43 for the showcase
   and infrastructure work).
2. Work on a feature branch; one writer per branch. Do not merge to `main`; do not force-push shared work; no releases or tags.
3. Before asking for review, run the build and tests in the README. For render changes record the build hash and whether the adapter was software Vulkan or a physical GPU.
4. Build with the CI flags when you touch C++: `-DCMAKE_BUILD_TYPE=Debug -DSAIGE_GFX_WARNINGS_AS_ERRORS=ON`. A Release tree with warnings off hid a CI-breaking `-Werror=double-promotion` once.
5. Showcase assets are fetched, not committed (`showcase/03-horizon/tools/fetch_assets.py`); tests that need them exit 77 (skip) when they are absent, so set `SAIGE_HORIZON_ASSETS`.
6. A regression test is not done until you have seen it fail against the defect (a mutation or a control). Restore mutated files from a copy and re-run; never leave a mutation in the tree.
7. A command that needs the owner (money, credentials, security permissions, a merge to `main`, a release) goes to the owner as a short, specific request on the issue. Never paste a secret anywhere.

## Where the character and physics work stands (2026-11-25)

Branch `claude/production-infrastructure`, CI green through run 593 (all three Graphics jobs). `development-state.md` has the per-feature record and `capabilities.md` the status table; checkpoints 1 to 5 are on issue #43. Not merged, no PR.

* Done and CI-validated: the humanoid animation module (`engine/animation`: locomotion, retargeting, IK, foot lock and `FootPlacement`, root-motion actions including vertical climbs, layering, look-at, hand IK, crouch), the matching additive Core fields (`CapsuleInput::traction`, `drive`, `crouch`; `CapsuleCharacterConfig::crouch_*`; `CapsuleCharacterState::crouched`), surface materials and `contact_velocity`, and the vehicle's handling options and powertrain. Every new Core field is additive and bit-identical when unused (the Showcase 03 replay checks are the guard).
* Needs an owner decision, not more code: strafing and turn-in-place (no clips in UAL or UAL 2; `mocap.cs.cmu.edu` does not resolve from the cloud session's network: allow that host or name another CC0 or properly licensed source), adopting a physics library (Jolt or Bullet; evaluation is in `docs/engineering/`), creating the scheduled run below, any merge to `main`.
* Open and unblocked but speculative until a product need picks them: a vehicle differential, damage state and engine braking, arm joint limits and body avoidance for hand IK, a second jump, crouch transitions, aligning a character to a ledge before a climb, replacing CesiumMan in a Showcase-class app (not Showcase 03).
* Pitfalls found the hard way: compare `CapsuleCharacterState` with `==`, never `memcmp` (padding); MSVC rounds differently from gcc, so never test `acos`-style distances of identical values against 0 (`rotation_angle_between` is exact for identical rotations); a CMake test target that links nothing needs `cxx_std_20` or MSVC builds it as C++14; do not push while a CI run is in progress (a new push cancels it); the physics envelope is +-256 m, so long vehicle tests start at x = -240.
* Assets are fetched, not committed: `python3 tools/animation/fetch_humanoid_assets.py DIR` (UAL, CMU, optional UAL 2) and `SAIGE_ANIM_ASSETS=DIR`; without them the real-asset tests skip.

## Scheduled engineering run (proposal; not enabled)

A cloud scheduled task could advance the backlog in bounded increments. Constraints it must carry: a single writer (one branch, `claude/engineering-run`, rebased from the target branch before each run);
a checkpoint report per run posted to issue #43 in the format in `master-directive.md`; at most one increment per run and a hard stop after the first failing test it cannot explain; no paid usage, no limit
workarounds, no scheduling of further runs from within a run; no main merges, releases or tags. It must not be described as enabled until it has been created and a first run has been observed.
