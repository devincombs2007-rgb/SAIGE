# Claude Code on SAIGE

`AGENTS.md` is the owner-approved governance and takes precedence over anything here. This file only tells a Claude Code session where the standing engineering material is.

* Standing directive and rules: `docs/engineering/master-directive.md`
* What exists and how far it is proven: `docs/engineering/capabilities.md`
* Dependency-ordered plan and backlog: `docs/engineering/roadmap.md`, `docs/engineering/backlog.md`
* Branch, PR and CI state: `docs/engineering/development-state.md`
* Picking the work up cold: `docs/engineering/agent-handoff.md`
* Showcase 03 defects and their guards: `docs/engineering/showcase-03-defects.md`

Build with the CI flags when touching C++ (`-DCMAKE_BUILD_TYPE=Debug -DSAIGE_GFX_WARNINGS_AS_ERRORS=ON`) and run `ctest`. No merges to `main`, no releases, no secrets in chat or on GitHub.
