# Running the moving-marker project from an agent

A built engine provides a process API. Run `saige_headless --help` to see its arguments. To get one UTF-8 JSON object on stdout, run a bounded session such as `saige_headless --json --frames 3`. Capture stderr for diagnostics and check the process exit code before reading the result.

```json
{"schema_version":1,"frames":3,"ticks":3,"instances":1,"marker_x":0.05}
```

Version 1 reports integer counts for completed frames, simulation ticks and instances in the last frame. The numeric marker_x is its x position in world units. A zero-frame run returns zeros for all four measurements. The example uses wall-clock pacing, so tick count and position may vary. Consumers should check schema_version, allow extra fields at the same version and treat unknown versions as unsupported. Omitting --json selects human-readable output.

The [sample project](../examples/moving-marker/saige.project.json) records the project format version, the built-in sample name and the unsigned 64-bit frame budget. Use the Python 3 standard-library launcher from the repository root:

```sh
python tools/agent/run_project.py examples/moving-marker/saige.project.json --engine ./build/saige_headless
```

Pass the path to the compiled executable for your build configuration, including its .exe suffix on Windows. Each v1 project contains exactly the three fields shown in the example. The launcher validates their types, rejects unknown samples and duplicate fields, and forwards the engine's stdout, stderr and exit status. Project data cannot select an executable. Exit 0 indicates success, 2 indicates invalid CLI arguments or project data, and 1 indicates launch or runtime failure. An MCP adapter can invoke the same command and decode its result.
