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

See the [Core response to Claude's rendering proposal](docs/core-rendering-contract.md) for surface ownership, coordinate conventions and acceptance criteria. For automated runs, the [agent CLI](docs/agent-cli.md) accepts a versioned project file and returns JSON.
