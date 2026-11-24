# SAIGE

Sensational Artificial Intelligent Game Engine is an independent C++20 game engine. The first executable boots a world, moves an entity at one unit per simulated second, and reports its last submitted position. It runs headless on a monotonic clock.

## Build and run

Use CMake 3.21 or newer and a C++20 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Linux with a single-configuration generator, run `./build/saige_headless --frames 60`. With a Visual Studio generator on Windows, run `.\build\Release\saige_headless.exe --frames 60`. The frame limit defaults to 60. The final line reports frames, simulation ticks, submitted instance count and the marker's x position. Elapsed wall time determines the tick count.

See the [architecture](docs/architecture.md) for the runtime and module contracts and [AGENTS.md](AGENTS.md) for ownership and review.

## Windows desktop milestone

With Visual Studio 2022, the C++ desktop workload, a recent Windows SDK, Python 3 for the CLI tests, and CMake 3.21 or newer, open a terminal at the repository root:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
.\build\windows-debug\Debug\saige_desktop.exe
```

The host opens a resizable native window. The simulation advances one marker automatically. WASD or the arrow keys move it in the horizontal plane. Escape or the close button ends the run and prints the final position. Pass `--frames 60` for a bounded run. The first visible 3D object is the next renderer integration.

For a Windows build that presents the moving cube, follow the [Vulkan build and acceptance instructions](docs/windows-renderer-integration.md).

See the [Core response to Claude's rendering proposal](docs/core-rendering-contract.md) for surface ownership, coordinate conventions and acceptance criteria. For automated runs, the [agent CLI](docs/agent-cli.md) accepts a versioned project file and returns JSON.

## Documentation

- [Architecture](docs/architecture.md) and [Core rendering contract](docs/core-rendering-contract.md)
- [Engine standards](docs/standards/), such as the coordinate system (right-handed, Z-up, meters)
- [Rendering and asset systems](docs/rendering/): RHI, Vulkan backend, render graph, shaders, asset pipeline


### Character course

Run `saige_desktop --character-course --require-rendered` from a Windows Vulkan build. Use WASD or the arrow keys to move,
Shift to sprint, and Space to jump. Drag with the right mouse button to orbit the camera. Movement follows the camera’s heading.
Press R to reset both the player and camera. Falling off the course also resets both. The prototype uses a fixed-step capsule
controller against static box geometry. The camera retracts at obstacles and eases back when clear. The player and obstacles use the sample cube
renderer. The course runs separately from saved scene projects. Use `--walk-speed` and `--sprint-speed` to tune movement in meters per second.


With project and graphics support enabled, launch `saige_desktop --character-course --course-collision
examples/character-course/course.collision.json --require-rendered` to play an authored course. The first box is the floor, with
the origin inside its top face at Z=0. The other boxes get cube markers at their base centers. Markers use the sample renderer’s
default dimensions. The course loads its collision file at startup. Live chunk loading is exercised separately by the streaming
tests.


For the live-streamed course, run:

```text
saige_desktop --character-course --course-world examples/textured-scene/course-world.json --require-rendered
```

The host starts in cell `[0, 0]` and keeps its reset floor resident. It stages nearby scene and collision files one chunk per
simulation tick. A completed candidate takes effect before movement on the next fixed step. Boundary guards keep the capsule
inside the current set while loading. If a neighbor fails to load or overlaps the capsule or spawn, the course keeps its last
working set. Press R to reset and retry nearby loads. The Vulkan course draws imported nodes from all resident chunks alongside the player marker. The host offsets project nodes
by the owning cell and publishes their GPU models with collision. The course keeps resident GPU models and uploads at most one new model per fixed update after startup. Uploads are synchronous.


Use `examples/textured-scene/ramp-course-world.json` with `--course-world` for the origin-relative ramp fixture. It combines box
and triangle collision with the same capsule and an authored checkpoint.

Add `--course-vehicle` for the flat driving prototype. Walk near the parked vehicle marker and press E to enter.
Use W/S to accelerate or reverse, A/D to steer, and Space to brake. Press E while stopped to exit when a supported side
is clear. R restores the player and vehicle. This prototype uses a kinematic body and flat box collision.
