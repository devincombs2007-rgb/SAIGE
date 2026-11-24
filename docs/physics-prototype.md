# Physics and the first playable prototype

The first playable target is a small streamed test course. A player walks and runs over steps and slopes, interacts with a
vehicle, drives across a chunk boundary, exits onto solid ground, and can reset the session. Use public engine fixtures. Keep
Brotherhood missions, characters, and creative assets in the game's private project.

## Delivery order

1. Run a fixed-step character course with a capsule, ground contact, acceleration and braking, slopes, step-up and jump.
   Capture movement input per tick. Use primitive static collision and draw the collision geometry alongside the visible scene.
2. Add a vehicle chassis and wheel contact, braking, steering, and a repeatable reset. Begin with a single vehicle and a
   repeatable obstacle course. Tune suspension and surface friction from telemetry.
3. Add enter/exit interactions and stable control handoff. Drive animation from grounded state, speed and turn rate. Add
   animation root motion only after ownership of gameplay movement and pose correction is explicit.
4. Run the course across two chunk boundaries. Stage collision alongside the scene and commit at a fixed-step boundary.
   Keep nearby collision active and publish the complete replacement before retiring old bodies. Failed loads retain collision.

## Runtime boundary

Core owns simulation ticks, stable entity identities, input commands, and world-space transforms. A physics implementation
owns body handles, contact caches, solver and broadphase state. Handle retirement follows entity generation and chunk
residency. Rendering reads snapshots and interpolates for display. The physics step uses compact coordinates local to an
active region. Defer coordinate rebasing until the two-chunk course passes and capture the limitations of the local space.

Evaluate an embedded library with a small C++ adapter or a focused prototype backend. Compare character sweeps, vehicle
contact and constraints, platform/compiler support, licensing, multithreading, profiling, and replay behavior before
choosing a dependency. Avoid a public generic physics abstraction until the character and vehicle both exercise the boundary.

## Acceptance evidence

- Replay the same recorded input twice in the same build and compare tick-indexed poses and contact states within documented
  tolerances. Record build, platform, fixed step, and tick count with the result.
- Test blocked and successful movement, step height, loss of contact, stable standstill, enter/exit, drive and brake, and reset.
- Cross a chunk boundary, return, and repeat with a missing collision candidate. Confirm stable identities and grounded state.
- Report CPU step time and peak and post-unload body counts for the fixed course. Verify keyboard input and visible movement in
  the native Windows sample with software Vulkan. Track physical hardware evidence separately.
