# SAIGE architecture

## First milestone

A game boots, owns a world, advances simulation, submits a frame and exits. The headless sample exercises the complete path with a real clock and a moving marker. The Win32 desktop host uses the same runtime. Its frame consumer reports the last pose. The next integration is a visible renderer. The proposed baseline is C++20, CMake and a single simulation thread.

## Module boundaries

| Module | Responsibility | Dependency direction |
| --- | --- | --- |
| World | Entity lifetime, position storage and snapshot extraction | Standalone C++ types |
| Runtime | Event pump, fixed-step updates and one frame submission per iteration | World and shared adapter interfaces |
| Application | Own world and adapters, select configuration, handle startup failure | Links saige_core and selected adapters |
| Platform adapter | Supply monotonic time, pump OS events and pace the host when needed | Implements IPlatform |
| Graphics adapter | Consume frame data, own device, shader and presentation resources | Implements IFrameSink |
| Asset cooker | Validate source assets, emit versioned runtime data | Runs offline, with loader agreement at the consuming adapter |

The public headers are the source-level contract. Build consumers with the same toolchain as the engine. Keep backend headers out of the public runtime API. The application constructs dependencies and keeps them alive until run() returns.

## Tick and frame contract

1. The runtime polls the platform. A quit response exits before the next update or submission.
2. Elapsed monotonic time accumulates toward a fixed simulation step, initially 16,666,667 ns. A frame can have zero or more ticks. The runtime processes at most eight ticks by default, drops excess whole ticks after a stall and preserves fractional time.
3. Each tick calls IGame::fixed_update with a mutable World. Handles carry a slot and generation and belong to one world. Destroying an entity invalidates its handle. The initial world stores position only.
4. After updates, the runtime extracts a snapshot in ascending slot order and calls IFrameSink::submit on the same thread. FrameView contains the completed tick count, the fraction toward the next tick in [0, 1), and a read-only span of entity positions. Tick zero and empty spans are valid. A sink that queues work must copy the data it needs before returning.

A finite frame budget supports smoke tests and batch tools. The sample uses a steady clock and sleeps between polls. Runtime tests inject a scripted clock so timing assertions are repeatable. This is a single-threaded source contract. Future asynchronous simulation or rendering will need explicit buffer ownership and synchronization.

## Decisions and extension order

- A snapshot gives Claude a testable rendering input today. The runtime reuses one CPU snapshot buffer within each run. The span is valid only during submit. Capacity tracks the high-water mark during a run. The snapshot still scans world slots and copies live instances. Profile those costs in a representative scene before changing storage.
- The [desktop contract](core-rendering-contract.md) records the Win32 surface lifetime, resize protocol and decisions in response to Claude’s Vulkan proposal. Platform and GPU resources live in their adapters.
- The first content milestone is one original mesh cooked offline and displayed by the sample. Define asset identity and coordinate conventions before extending RenderInstance.
- Physics, animation, NPCs and weather can begin as fixed-update systems as their first use cases arrive. Open-world streaming will need persistent object IDs and versioned assets. Current entity handles are local to a live world.
- AI tools can run version 1 of the [headless CLI and project file](agent-cli.md). A future MCP server can call this interface. Authoring operations and replay inputs require their own versioned contracts.

See [first development tasks](first-tasks.md) for owners and acceptance criteria.
