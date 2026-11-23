# Performance measurement for the 60 FPS goal

Issue #62 sets a goal of 60 FPS (16.67 ms per frame, including CPU simulation, world streaming and GPU rendering) on a target
machine, resolution and graphics preset that are still to be chosen. This module (`include/saige/perf.hpp`) makes the CPU side
repeatable and keeps measurements from being over-read. **It does not show that the goal is met.** Software-Vulkan runs validate
correctness, not hardware performance, and the target hardware is not yet documented.

## What each number means

| Report field | Source | Meaning |
| --- | --- | --- |
| `frame_ms` | `CpuFrameMetrics::total` | CPU time of one `saige::run()` iteration: event polling, fixed ticks, snapshot extraction and `IFrameSink::submit()`. If the sink blocks on presentation or on the GPU, that wait is inside `submission` and therefore inside this total. |
| `average_fps` | `1000 / mean(frame_ms)` | Frames per second implied by CPU frame time. It overstates real throughput when the loop is paced or idle. |
| `wall_interval_ms`, `wall_average_fps` | steady clock between the ends of consecutive frames | The time a viewer actually experiences, including pacing waits. Use this, not `frame_ms`, for delivered FPS. |
| `phases_ms.simulation`, `simulation_per_tick` | `CpuFrameMetrics::simulation` | All fixed ticks in the frame, and that total divided by ticks run. This is the **fixed-step simulation cost**. |
| `zones_ms.<name>` | `Zones::record` / `Zones::scope` | Any named subsystem. `TimedGame` records every `fixed_update` as `fixed_step`; the course benchmark adds `physics.step`, `chunk.stage_next` and `chunk.commit_frame`. |
| `budget.frames_over_budget`, `frames_over_2x_budget` | counts above 16.67 ms and 33.33 ms | Hitches. Compare with `p99_ms` and `max_ms`; a single stall shows in `max_ms` and the counters long before it moves a percentile. |
| `process_memory_bytes` | `/proc/self/status` or `GetProcessMemoryInfo` | Working set and its high-water mark, sampled every 60 frames. Empty (`null`) where unsupported. |

Percentiles are nearest-rank (rank = ceil(n x p / 100)), the definition `saige_scene_trace` already used. With fewer than 100 frames
p99 is the maximum; judge percentiles on runs of at least several thousand frames.

## Verdict rules

`budget.verdict` is `not_evaluated` unless **all** of these hold: `adapter_class` is `physical`, and the caller set
`qualified_target_hardware` for a run on the documented target hardware, resolution and preset. Only then is it `within_budget`
(p99 frame time at or below the budget) or `over_budget`. `adapter_class` is `none` when no renderer ran, `software` for a
software rasterizer (lavapipe, llvmpipe) and `physical` for a hardware GPU. Never merge software and physical reports into one
table; the adapter class is part of every report so they cannot be confused.

## Determinism

The observer and `TimedGame` only read the steady clock. They never feed time back into simulation, so a recorded run produces
the same ticks, poses and snapshots as an unrecorded one (`tests/perf_test.cpp` compares them). Fixed-step cost is reported per tick
so it stays comparable when the render rate changes the number of ticks per frame.

## Using it

```cpp
saige::perf::FrameRecorder recorder({.reserve_frames = 100000});
saige::perf::Zones zones;
saige::perf::TimedGame timed(game, zones);          // optional: times each fixed tick
saige::run(world, platform, timed, sink, frames, config, &recorder);
std::cout << saige::perf::to_json(recorder, zones, {"my_workload", "Release", "none", false, 16.6667, config.fixed_step.count()});
```

GPU pass timings and device memory are not duplicated here; they come from `RendererFrameSink::diagnostics()` and apply only on
adapters that report timestamps. Record them next to this report with the same adapter class.
