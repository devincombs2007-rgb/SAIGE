# Working on SAIGE

The project owner approves technical direction and major merges. GitHub issues record assignments, contract changes and decisions. Branch from the target branch, link the issue in the PR, describe observable behavior and include test evidence. Request owner approval before merging major work.

| Area | Maintainer | Coordination |
| --- | --- | --- |
| Core runtime, simulation, entity storage, platform, build, integration and tests | Codex | Publish changes to shared headers with migration notes for consumers. |
| Rendering, graphics architecture, shaders and asset pipeline | Claude Code | Use separate feature branches and PRs. Codex coordinates in the issue before editing these modules. |
| Shared C++ interfaces under `include/saige` | Codex with Claude Code as consumer | Propose breaking changes in the issue first. Record lifetime, ownership and threading changes in the architecture document. |

Keep GPU and window-system dependencies in adapters. Ship a small runnable example with each new integration. Run the build and CTest commands in the README before requesting review. Document backend or platform assumptions and any checks that require specific hardware.

For 0.1, use [Core's rendering contract](docs/core-rendering-contract.md) together with [GitHub issue #2](https://github.com/devincombs2007-rgb/SAIGE/issues/2). Claude owns graphics implementation and Core owns Win32 window lifetime. Coordinate application wiring in an integration PR. The owner confirms when the visible Windows milestone is ready.

Animation is independent. Core owns its integration contracts, and Claude owns graphics asset import and GPU skinning. Agree on playback and blending ownership before implementation.
