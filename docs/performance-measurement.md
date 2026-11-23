# Performance measurement for the 60 FPS goal

## The goal

Issue #62 records the owner's provisional acceptance target for the first playable character and vehicle course:

- Windows 11 desktop, AMD Ryzen 5 5600, NVIDIA GeForce RTX 3060 12 GB (physical GPU), 16 GB RAM, SSD.
- 1920x1080 native, a documented and reproducible **High** preset, ray tracing, upscaling and frame generation all off.
- **p99 delivered frame interval of at most 16.67 ms** (about 60 FPS) on a representative scripted path (walking, jumping,
  collisions, chunk crossing, and vehicles once implemented), measured as actual delivered frame pacing, not an unpaced CPU loop.
- Record mean, p50, p95, p99 and max, hitch counts, CPU physics and streaming zones, GPU pass times where available, and memory.
- The High preset and the course fixture must be **versioned** before formal qualification. A 1440p stretch target comes later.

This is a goal, not a result. Nothing in this repository has run on that hardware, software-Vulkan CI validates correctness and
not performance, and the headless course benchmark is an unpaced CPU loop. Reports therefore say `not_evaluated` until a run
meets every condition in "Verdict rules" below.

## What each number means

| Report field | Source | Meaning |
| --- | --- | --- |
| `frame_interval_ms`, `interval_average_fps` | wall-clock time between consecutive frames (or intervals the host supplies with `add_frame_interval`) | **The goal's statistic.** What a player sees, including any presentation pacing wait. Only meaningful when `run.paced` is true. |
| `frame_ms`, `cpu_average_fps` | `CpuFrameMetrics::total` | CPU time of one `saige::run()` iteration: polling, fixed ticks, snapshot extraction and `IFrameSink::submit()`. Reported beside the interval, never in its place. The FPS derived from it overstates throughput when the loop is paced or idle. |
| `phases_ms.simulation`, `simulation_per_tick` | `CpuFrameMetrics::simulation` | All fixed ticks in the frame, and that total divided by ticks run: the **fixed-step simulation cost**. |
| `zones_ms.<name>` | `Zones::record` / `Zones::scope` | Any named subsystem. `TimedGame` records every `fixed_update` as `fixed_step`; the course benchmark adds `physics.step`, `chunk.request`, `chunk.stage_next` and `chunk.commit_frame`. |
| `budget.intervals_over_budget`, `intervals_over_2x_budget`, `cpu_frames_over_budget` | counts above 16.67 ms and 33.33 ms | Hitches. A single stall shows in `max_ms` and these counters long before it moves a percentile. |
| `process_memory_bytes` | `/proc/self/status` or `GetProcessMemoryInfo` | Working set and its high-water mark, sampled every 60 frames. `null` where unsupported. |
| `run.dropped_timing_samples` | `Zones::dropped_samples()` | Timing samples a scope had to discard because memory ran out. Zero in a healthy run. |

Percentiles are nearest-rank (rank = ceil(n x p / 100)), the definition `saige_scene_trace` already used. With fewer than 100 frames
p99 is the maximum; judge percentiles on runs of at least several thousand frames.

## Verdict rules

`budget.verdict` is `not_evaluated` unless **all** of the following hold, and is then `within_budget` or `over_budget` by whether the
p99 of `frame_interval_ms` is at or below the budget:

1. `adapter_class` is `physical` (`none` means no renderer, `software` a software rasterizer such as lavapipe or llvmpipe);
2. `paced` is set: frames were presented at the display's pace;
3. `qualified_target_hardware` is set for a run on the documented target hardware, resolution and preset;
4. both `preset_version` and `course_version` are named;
5. at least one frame interval was recorded.

`verdict_reason` names the first missing condition. Never merge software and physical reports into one table; the adapter class is
part of every report so they cannot be confused.

## Determinism

The observer and `TimedGame` only read the steady clock. They never feed time back into simulation, so a recorded run produces the
same ticks, poses and snapshots as an unrecorded one (`tests/perf_test.cpp` compares them). Fixed-step cost is reported per tick so
it stays comparable when the render rate changes the number of ticks per frame.

## Exception safety

- `Zones::Scope`: construction may allocate (it resolves its series) and may throw `std::bad_alloc`, before anything is timed.
  Its destructor is `noexcept` and **never throws**: if the series cannot grow, the sample is dropped and counted in
  `dropped_samples()`. This matters most when the scope is unwinding because the timed code threw; throwing from there would
  call `std::terminate`. `TimedGame` therefore times a tick that throws and lets the original exception propagate.
- Call `Zones::reserve(name, n)` for each zone used in a fixed-step loop and a scope on it never allocates, so recording there is
  allocation-free as well as non-throwing.
- `FrameRecorder::record` makes room in every series before it changes anything: if it throws, the recorder is exactly as it was,
  and afterwards recording cannot fail. Process-memory sampling is best effort and cannot fail a frame. `add_frame_interval` has the
  strong guarantee.
- `tests/perf_test.cpp` proves these with allocation-failure injection (not built on MSVC, where replacing the global operator new in
  a test executable is skipped); disabling the destructor guard makes the test abort.

## Using it

```cpp
saige::perf::FrameRecorder recorder({.reserve_frames = 100000});
saige::perf::Zones zones;
zones.reserve("fixed_step", 100000);
saige::perf::TimedGame timed(game, zones);          // optional: times each fixed tick
saige::run(world, platform, timed, sink, frames, config, &recorder);
saige::perf::ReportContext context{"my_workload", "Release", "none"};
context.fixed_step_ns = config.fixed_step.count();
std::cout << saige::perf::to_json(recorder, zones, context);
```

GPU pass timings and device memory are not duplicated here; they come from `RendererFrameSink::diagnostics()` on adapters that
report timestamps. Record them next to this report with the same adapter class.
