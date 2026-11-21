# Working on SAIGE

The project owner approves technical direction and major merges. GitHub issues record assignments, contract changes and decisions. Branch from the target branch, link the issue in the PR, describe observable behavior and include test evidence. Request owner approval before merging major work.

| Area | Maintainer | Coordination |
| --- | --- | --- |
| Core runtime, simulation, entity storage, platform, build, integration and tests | Codex | Publish changes to shared headers with migration notes for consumers. |
| Rendering, graphics architecture, shaders and asset pipeline | Claude Code | Use separate feature branches and PRs. Codex coordinates in the issue before editing these modules. |
| Shared C++ interfaces under `include/saige` | Codex with Claude Code as consumer | Propose breaking changes in the issue first. Record lifetime, ownership and threading changes in the architecture document. |

Keep GPU and window-system dependencies in adapters. Ship a small runnable example with each new integration. Run the build and CTest commands in the README before requesting review. Document backend or platform assumptions and any checks that require specific hardware.
