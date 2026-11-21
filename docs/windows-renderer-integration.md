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

## Windows software verification

The [Windows Lavapipe job](https://github.com/devincombs2007-rgb/SAIGE/actions/workflows/windows-software-vulkan.yml) runs on the standard public Windows runner. It installs the Vulkan loader and validation layer through MSYS2 and verifies the checksum of the Mesa ICD before using it. The runner is disposable. Its Vulkan manifests are registered for that job because elevated processes can ignore the environment overrides. The runtime installer and ICD are test dependencies and are not included in the SAIGE executable archive.

The job records the selected CPU device and requires Vulkan validation. It checks that the moving cube changes position in offscreen readback and uploads first and last frames. A separate native Win32 test exercises surface creation, presentation, resize, minimize/restore, keyboard messages and shutdown. The job also boots the product executable in strict mode. The captures are offscreen readbacks. The Win32 test exercises swapchain presentation but does not capture the displayed window.

## Physical GPU verification

A physical-driver acceptance run requires an interactive Windows x64 session with a Vulkan 1.3 hardware driver and the Khronos validation layer. A trusted volunteer can use their existing hardware and post results directly in issue #9. For automated testing, configure with `-DSAIGE_TEST_WIN32_VULKAN=ON`, run `ctest --preset windows-release -V -R "vulkan|win32_platform"`, and retain a log that shows the real adapter and `software=false`. Then run `saige_desktop.exe --require-rendered --validate` and record a native window capture showing motion and resize. Include the GPU and driver version, clean shutdown, and validation output. Do not use the Lavapipe override on this run.

| Option | Fit |
| --- | --- |
| Standard public GitHub Windows runner with Lavapipe | Free software and Win32 presentation coverage. This is the automated default. |
| SwiftShader | An alternate Windows CPU Vulkan 1.3 implementation if Lavapipe becomes unavailable. It still requires checking SAIGE's required features, Win32 surface support, and validation compatibility. [Upstream](https://swiftshader.googlesource.com/SwiftShader.git/) |
| GitHub Windows GPU larger runner | Windows Tesla T4 is available on eligible plans, but larger runner time is billed even for public repositories. [Runner types](https://docs.github.com/en/actions/reference/runners/larger-runners) and [pricing](https://docs.github.com/en/billing/reference/actions-runner-pricing) |
| GitLab hosted GPU runners | GPU instances are Linux only. The standard Windows VM is no substitute for the Win32 hardware gate. [GitLab documentation](https://docs.gitlab.com/ci/runners/hosted_runners/gpu_enabled/) |
| Azure GPU Windows VM | An NVadsA10 v5 virtual workstation with the supported driver is a candidate after approval. The free Windows B1s VM is not a GPU tier. Confirm quota, cost and interactive Vulkan presentation before provisioning. [GPU VM](https://learn.microsoft.com/en-us/azure/virtual-machines/nva10v5-series) and [free tier](https://learn.microsoft.com/en-us/azure/cost-management-billing/manage/create-free-services) |

No free hosted Windows hardware runner has been verified for this repository. Keep the GPU gate open until there is a qualifying run.

## Release gate

After review and the Windows hardware run, build from the release commit, rerun the core, sanitizer, software and hardware checks, and tag `v0.1.0`. Attach the native Windows x64 executables, SHA-256 checksums, capture and CI links, build instructions, and the limitations above. Preview artifacts can be used before this gate. Ship the Vulkan test driver separately from the application by leaving it in CI.
