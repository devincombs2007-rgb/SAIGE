# Master engineering directive (durable summary)

Source: the SAIGE MASTER ENGINEERING DIRECTIVE, issued through the development director with the project owner's authorization while Codex is unavailable. This page records its standing content so that any
agent can pick the work up. It does **not** replace `AGENTS.md`, which stays the owner-approved governance; where the two differ, `AGENTS.md` and the owner win.

## Mandate

Claude Code carries Core and Graphics together while Codex is unavailable, as a sustained, checkpointed program. Priorities are flexible; the starting weights are physics 22%, core architecture and
optimization 21%, characters and animation 20%, assets and tools 13%, world simulation and AI 10%, graphics 7%, audio 7%.

## Standards

* Research, design, implement, test (correctness, integration, performance), document, accept. Use sanitizers, Vulkan validation and mutation checks where they apply.
* Never manipulate a test to pass. A regression test must be shown to fail against the defect it guards (a mutation or control).
* Record exact commits, test commands and hardware class. Software Vulkan (lavapipe) results are never reported as physical GPU performance. Capability-gated features (ray tracing, DLSS, FSR, XeSS) are not
  labelled operational until tested on hardware that has them.
* Separate what is implemented from what is proposed in every report.

## Authorization

Authorized: research, install free development tools, acquire properly licensed assets, implement, test, document, push reversible work to branches.

Not authorized: spending money, exposing secrets, bypassing restrictions or bot protection, publishing restricted assets, force-pushing shared work, merging to `main`, publishing releases or tags.

Escalate to the owner for: spending, credentials, security permissions, major architectural incompatibility, irreversible changes, licensing uncertainty, major dependency adoption, merges to `main`,
releases. Never ask for or expose credentials in chat or on GitHub; use the environment's secret mechanism. The engine repository is public: do not commit assets whose licenses prohibit redistribution.

## Showcase rules

Showcase 03 stays as delivered (film build commit 9c87457): no remake, no re-render. Showcase 04 is not authorized and has no deadline. Defects found in 03 are root-caused by instrumentation, fixed in reusable
engine or app code with regression tests, and never hidden by camera or choreography changes (see `showcase-03-defects.md`).

## Checkpoint report (every run)

Implemented; corrected; validated; exact branch and commit; tests passed, failed, skipped; architectural decisions; limitations; new defects; required owner actions; next assignment.
