# SAIGE 0.1 Windows rendering acceptance

Core owns the Win32 host and sample entry point. Rendering owns the Vulkan backend and frame sink. The first visible target is a lit cube on a procedural ground plane that moves with the simulation. The headless and Windows host builds remain available without Vulkan. See the [shared contract](core-rendering-contract.md) and [integration issue](https://github.com/devincombs2007-rgb/SAIGE/issues/9).

## Host handoff

Construct Win32Platform with DesktopPresentation::renderer_paced for a presenting sink. This suppresses system background erasure and lets presentation pace drawable frames. The host continues to pace minimized and zero-extent windows. If the renderer repeatedly skips a drawable frame while waiting for swapchain recovery, it must bound retries and yield. The default constructor keeps host pacing for nonpresenting consumers.

Read SurfaceState inside submit after the message pump. Distinguish minimized/zero extent from recoverable swapchain status. Resize may become visible after the border drag releases because Windows runs a modal size loop. Keep the platform alive until the sink has released all swapchain and surface resources. Keep the existing console-probe build and clearly document the Vulkan-enabled command separately.

## Acceptance evidence

1. Use the null backend to verify draw recording and resource lifetimes. Run offscreen Vulkan with validation and software rasterization in CI, and assert that geometry is present in the readback. Compare two frames with a fixed camera to verify movement. Null-backend draws only verify recording.
2. Build the Vulkan-enabled executable with MSVC x64. Record which runtime and driver the test uses. On a Windows graphics runner, capture a presented frame and exercise resize, minimize/restore, held WASD, focus loss, Escape, and title-bar close. Verify validation stays free of errors and the process exits. If a graphics runner is unavailable, record the gap alongside the artifact.
3. Upload the tested executable and capture with the source commit. Include Vulkan startup prerequisites and useful diagnostics for an absent loader or unsupported device.

For 0.1, Core copies the world snapshot once per frame and applies held input to fixed simulation ticks. Continuous rendering during modal resize, interpolation history, reusable snapshot buffers, replayable input transitions, and GPU device-loss recovery are follow-up work. Profile snapshot size and extraction time with a representative scene before changing its storage.

## Build the presenting executable

From a PowerShell terminal with Visual Studio 2022 and CMake 3.21 or newer, run:

```powershell
cmake --preset windows-release -DSAIGE_ENABLE_VULKAN=ON -DSAIGE_VULKAN_FETCH_HEADERS=ON -DVULKAN_HEADERS_ENABLE_MODULE=OFF
cmake --build --preset windows-release
ctest --preset windows-release
.\build\windows-release\Release\saige_desktop.exe --require-rendered
```

A Vulkan 1.3 driver and loader are required at runtime. Without the strict flag the app can fall back to the console probe when the GPU is unavailable. With the Vulkan validation layer installed, add `--validate` and capture stderr. Use `--frames 240` for a bounded smoke run. It reports render and skip counts on exit and fails if no frame was rendered. A native acceptance run should still observe the displayed cube and exercise the window and input transitions listed above. CI can compile and package the executable even if the Windows runner has no Vulkan driver. The Linux graphics workflow provides separate offscreen and X11 presentation evidence.
