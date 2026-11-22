# Renderer diagnostics (schema version 1)

`RendererFrameSink::diagnostics()` returns a `RendererDiagnostics`; `to_json()` prints it. Core's CPU frame metrics stay in
their own output: this one carries only what the renderer and device can measure. **A value the device cannot measure is
`null`, never zero.**

```json
{"schema_version":1,"adapter":"llvmpipe (LLVM 20.1.2, 256 bits)","software_device":true,"gpu_timestamps_supported":true,
 "gpu_scene_pass_ms":0.324927,"gpu_timing_frame":5,
 "device_memory":{"allocated_bytes":1048576,"allocation_count":14,"budget_bytes":null,"usage_bytes":null},
 "resources":{"live":11,"live_bytes":4096,"peak_bytes":8192,"created":19,"destroyed":8},"validation_errors":0}
```

| Field | Meaning |
| --- | --- |
| `adapter`, `software_device` | The device's name, and whether it is a CPU implementation. On a software adapter the "GPU" timings are CPU time: the name is there so a report is never mistaken for hardware evidence. Physical-GPU verification is tracked in #9. |
| `gpu_timestamps_supported` | The graphics queue writes timestamps (`timestampValidBits > 0`, non-zero period). |
| `gpu_scene_pass_ms`, `gpu_timing_frame` | Duration between two GPU timestamps around the scene pass (`timestamp_scene_begin` / `timestamp_scene_end`) of the latest frame whose work has completed and been collected, and which frame that was. Collection happens when a later frame reuses that frame's slot, so it lags by up to `frames_in_flight` frames. `null` before any frame has completed, and on devices without timestamps. Only the scene pass is timed; swapchain acquire, presentation and CPU work are not included. |
| `device_memory.allocated_bytes`, `allocation_count` | Device memory the backend itself allocated (exact: every allocation and free goes through one accounting point), at driver-aligned sizes. Staging buffers are freed after an upload and are not counted afterwards. Destroyed resources stay counted until the GPU has finished with them, then drop, so compare after `wait_idle` and a frame. |
| `device_memory.budget_bytes`, `usage_bytes` | The driver's budget and this process's usage of device-local memory (`VK_EXT_memory_budget`). `null` where the driver does not report it. The code path is exercised on the software adapter only; it has not been run on a physical GPU. |
| `resources` | The registry's totals: live resources and their *estimated* bytes (tightly packed mip chains and buffer sizes), peak, created and destroyed. |
| `validation_errors` | Backend validation errors since creation. |

Not measured (and therefore not reported): per-pass GPU times beyond the scene pass, GPU memory residency, driver-internal
allocations, and anything on a physical device.
