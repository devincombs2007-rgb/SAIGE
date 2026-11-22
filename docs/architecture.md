# SAIGE architecture

## Engine scope

SAIGE is an independently owned C++ engine for multiple games. Project Brotherhood is the first production target and will exercise large streamed worlds, vehicles, persistent simulation, and cinematic gameplay. Its proprietary content and creative work belong outside the public engine repository. Engine interfaces must serve multiple games. Use original public sample assets and synthetic workloads for reproducible verification.

The 0.1 preview booted a Win32 host with Vulkan rendering. The 0.2 target adds saved projects, hierarchical transforms, shared glTF assets, textured geometry and a controllable perspective camera. Track acceptance and review evidence in [issue #19](https://github.com/devincombs2007-rgb/SAIGE/issues/19) and shared interfaces in [issue #17](https://github.com/devincombs2007-rgb/SAIGE/issues/17).

## Module boundaries

| Module | Responsibility | Dependency direction |
| --- | --- | --- |
| World | Entity lifetime, render poses and snapshot extraction | Standalone C++ types |
| Runtime | Event pump, fixed-step updates and one frame submission per iteration | World and shared adapter interfaces |
| Application | Own world and adapters, select configuration, handle startup failure | Links saige_core and selected adapters |
| Platform adapter | Supply monotonic time, pump OS events and pace the host when needed | Implements IPlatform |
| Graphics adapter | Consume frame data, own device, shader and presentation resources | Implements IFrameSink |
| Asset cooker | Validate source assets, emit versioned runtime data | Runs offline, with loader agreement at the consuming adapter |

The public headers are the source-level contract. Build consumers with the same toolchain as the engine. Keep backend headers out of the public runtime API. The application constructs dependencies and keeps them alive until run() returns.

## Tick and frame contract

1. The runtime polls the platform. A quit response exits before the next update or submission.
2. Elapsed monotonic time accumulates toward a fixed simulation step, initially 16,666,667 ns. A frame can have zero or more ticks. The runtime processes at most eight ticks by default, drops excess whole ticks after a stall and preserves fractional time.
3. Each tick calls IGame::fixed_update with a mutable World. Handles carry a slot and generation and belong to one world. Destroying an entity invalidates its handle. Persisted IDs and runtime handles have separate lifetimes.
4. After updates, the runtime extracts a snapshot in ascending slot order and calls IFrameSink::submit on the same thread. FrameView contains the completed tick count, the fraction toward the next tick in [0, 1), a read-only span of render instances, and an optional camera. Tick zero and empty spans are valid. A sink that queues work must copy the data it needs before returning.

A finite frame budget supports smoke tests and batch tools. The sample uses a steady clock and sleeps between polls. Runtime tests inject a scripted clock so timing assertions are repeatable. This is a single-threaded source contract. Future asynchronous simulation or rendering will need explicit buffer ownership and synchronization.

## Decisions and extension order

- A snapshot gives Claude a testable rendering input today. The runtime reuses one CPU snapshot buffer within each run. The span is valid only during submit. Capacity tracks the high-water mark during a run. The snapshot still scans world slots and copies live instances. Profile those costs in a representative scene before changing storage.
- The [desktop contract](core-rendering-contract.md) records the Win32 surface lifetime, resize protocol and decisions in response to Claude’s Vulkan proposal. Platform and GPU resources live in their adapters.
- Persist stable asset IDs and use the shared [coordinate convention](standards/coordinate-system.md). Render instances borrow the snapshot and carry a mesh ID and an optional authoritative world affine transform.
- Physics, animation, NPCs and weather can begin as fixed-update systems as their first use cases arrive. Open-world streaming will need persistent object IDs and versioned assets. Current entity handles are local to a live world.
- AI tools can run version 1 of the [headless CLI and project file](agent-cli.md). A future MCP server can call this interface. Authoring operations and replay inputs require their own versioned contracts.

## Production roadmap

1. **Stabilize 0.2.** Verify native textured rendering, depth, input, scene persistence, transactional replacement, resource retirement and diagnostics. Preserve the 0.1 release and repeatable build and capture evidence. Keep Windows software rendering and physical GPU qualifications separate.
2. **Establish authoring and streaming contracts.** Version project and cooked asset schemas separately from the engine and renderer. Keep previous fixtures and test migrations from declared supported versions. Establish asset dependency and cancellation semantics, chunk ownership, memory budgets and representative load tests before implementing asynchronous streaming. Validate a large-world coordinate strategy at chunk and rendering boundaries.
3. **Expand simulation in measured slices.** Add explicit scheduling, resource lifetimes and threading ownership before parallel work. Integrate physics and animation on the fixed-step world, then build vehicle, intelligent NPC and persistent simulation workloads. Layer environmental simulation, spatial audio, cinematic tools and world editing on stable scene and streaming contracts.
4. **Evolve rendering alongside production samples.** Evaluate Vulkan 1.4 after 0.2 stabilization using actual capability and driver coverage with a supported fallback path. Keep Vulkan, future DirectX 12 and vendor dependencies in independently buildable adapters. Use explicit capabilities and optional integration for ray tracing, path tracing, DLSS, Ray Reconstruction, frame generation, NVIDIA Reflex, AMD FSR and Intel XeSS. Keep scene data, save formats and simulation independent of these decisions.

Use newer stable toolchains and SDKs when their release support and CI coverage justify an upgrade. Minimize transitive dependencies so an adapter or SDK update need only rebuild the affected targets. The current source contracts still require compatible toolchains. Introduce binary versioning only where independently shipped modules need it.

Review third-party dependencies for supported versions, license and redistribution terms, security maintenance, measurable workload cost and replacement options. Pin inputs and retain provenance for reproducible builds. Use performance and memory baselines for representative workloads before changing storage, threading, or rendering defaults.

Version structured project formats, CLI outputs and diagnostics as contracts for people and agents. Build automated correctness, migration, and performance checks on those contracts. Add agent-accessible development tools and MCP adapters to existing safe operations as authoring workflows mature.

See [first development tasks](first-tasks.md) for the original foundation scope.
