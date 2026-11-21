# First development tasks

The [Core integration decisions](core-rendering-contract.md) answer [Claude’s interface issue](https://github.com/devincombs2007-rgb/SAIGE/issues/2). Review each implementation on its feature branch.

## Codex — Desktop window and quit handling

Owner: Codex. Validate the Win32 adapter on a native Windows runner and integrate the graphics adapter after its own review.

Acceptance: the desktop host creates one window, pumps resize and close events on the owning thread, and exits cleanly on close. Expose the minimum agreed surface hook to the graphics adapter, add a lifecycle test, and keep the headless build usable without a display.

## Claude Code — Present the first frame

Owner: Claude Code. Use include/saige/runtime.hpp and include/saige/desktop.hpp to implement the first graphics backend on a separate rendering branch under [RHI issue #3](https://github.com/devincombs2007-rgb/SAIGE/issues/3). Record the backend, shader tooling, supported OS and surface requirements in this issue. Coordinate the surface contract with Codex before editing shared headers.

Acceptance: a desktop sample draws a visible primitive positioned from a RenderInstance, handles empty frames, resize and shutdown, and has a documented run command and captured frame. The backend owns its GPU resources and any data it retains past submit(). Keep GPU dependencies optional for the headless target.

## Claude Code — Cook one original mesh

Owner: Claude Code. In a separate asset-pipeline branch, choose a tiny original mesh fixture and document its units, coordinate conventions, format version and validation rules. Agree with Codex on asset identity before changing the shared frame contract.

Acceptance: one documented command cooks the fixture, a repeat cook produces identical output, a test round-trips its vertices and indices, and a malformed fixture produces a clear error. Integrate it into the first-frame sample after the renderer is ready.
