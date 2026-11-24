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

## Scheduled engineering run (proposal; not enabled)

A cloud scheduled task could advance the backlog in bounded increments. Constraints it must carry: a single writer (one branch, `claude/engineering-run`, rebased from the target branch before each run);
a checkpoint report per run posted to issue #43 in the format in `master-directive.md`; at most one increment per run and a hard stop after the first failing test it cannot explain; no paid usage, no limit
workarounds, no scheduling of further runs from within a run; no main merges, releases or tags. It must not be described as enabled until it has been created and a first run has been observed.
