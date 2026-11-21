# SAIGE

Sensational Artificial Intelligent Game Engine is an independent C++20 game engine. The first executable boots a world, moves an entity at one unit per simulated second, and reports its last submitted position. It runs headless on a monotonic clock.

## Build and run

Use CMake 3.20 or newer and a C++20 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

On Linux with a single-configuration generator, run `./build/saige_headless --frames 60`. With a Visual Studio generator on Windows, run `.\build\Release\saige_headless.exe --frames 60`. The frame limit defaults to 60. The final line reports frames, simulation ticks, submitted instance count and the marker's x position. Elapsed wall time determines the tick count.

See the [architecture](docs/architecture.md) for the runtime and module contracts and [AGENTS.md](AGENTS.md) for ownership and review.
