# Scene diagnostics in 0.2

Run a Release build from the repository root:

```sh
saige_scene_trace examples/textured-scene/saige.scene.json 1200
python3 tools/benchmarks/make_project.py /tmp/saige-8192.json --nodes 8192
saige_scene_trace /tmp/saige-8192.json 1200
```

The trace opens the saved project, advances one fixed simulation tick per frame, applies a scripted camera move, and records the CPU frame and extraction percentiles. It does not decode assets or present. The work and final camera pose are repeatable. The project_open_ms field measures CPU wall time for reading, parsing, validating, resolving the hierarchy, and creating the runtime world. It is separate from frame percentiles. Time measurements depend on the host and build. Keep the fixture, frame count, compiler configuration and host comparable across runs.

The optional runtime observer measures wall time with a steady clock. It reports event polling, simulation, snapshot extraction, and synchronous sink submission. In a presenting app, polling or submission can include pacing and driver waits. They should never be reported as CPU execution time or GPU duration. The snapshot payload counts object storage, excluding vector reserve, project data, and allocator overhead. The trace reports GPU time and device memory as null.

On a shared Linux Xeon Platinum 8370C runner, GCC 14.2 Release, a 1,200-frame trace with 8,192 instances measured 0.186 ms frame p95 and 0.167 ms extraction p95. The snapshot payload was 851,968 bytes. This run had no window or rendering backend. It measures per-frame tracing of an already loaded project, separate from the whole-hierarchy resolution benchmark.

## Renderer diagnostics contract

Measure rendering with device timestamp queries bracketing the same pass on a queue with nonzero timestampValidBits. Publish only completed samples. Account for valid-bit wraparound and convert ticks using timestampPeriod. Record sample count, adapter and driver, physical or software Vulkan, frame count, surface size, and submission mode with the percentile. Report unavailable values as null. Registry measurements should distinguish logical resource payload from driver budget/usage and retain a baseline before loading, after loading, and after retirement. Do not compare a CPU trace, a software Vulkan run and a physical GPU run as equivalent.

Run synchronization validation in both software lanes. A missing validation layer should fail those designated runs. Software Vulkan results prove renderer behavior for the recorded configuration only. Hardware verification stays separate in GitHub issue #9.
