# First development tasks

Track each task in its own GitHub issue and feature PR. Desktop and graphics planning can start together. Review shared surface requirements before either adapter depends on them.

## Codex — Desktop window and quit handling

Owner: Codex. Implement a desktop IPlatform adapter in a dedicated branch after the core foundation. Agree with Claude in this issue on the initial OS, window library and surface lifetime before adding dependencies.

Acceptance: the desktop host creates one window, pumps resize and close events on the owning thread, and exits cleanly on close. Expose the minimum agreed surface hook to the graphics adapter, add a lifecycle test, and keep the headless build usable without a display.

## Claude Code — Present the first frame

Owner: Claude Code. Review include/saige/runtime.hpp on core/runtime-foundation and implement the first graphics backend on a separate rendering branch. Record the backend, shader tooling, supported OS and surface requirements in this issue. Coordinate the surface contract with Codex before editing shared headers.

Acceptance: a desktop sample draws a visible primitive positioned from a RenderInstance, handles empty frames, resize and shutdown, and has a documented run command and captured frame. The backend owns its GPU resources and any data it retains past submit(). Keep GPU dependencies optional for the headless target.

## Claude Code — Cook one original mesh

Owner: Claude Code. In a separate asset-pipeline branch, choose a tiny original mesh fixture and document its units, coordinate conventions, format version and validation rules. Agree with Codex on asset identity before changing the shared frame contract.

Acceptance: one documented command cooks the fixture, a repeat cook produces identical output, a test round-trips its vertices and indices, and a malformed fixture produces a clear error. Integrate it into the first-frame sample after the renderer is ready.
