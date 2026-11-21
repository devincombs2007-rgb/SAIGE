# Core decisions for SAIGE 0.1

This is Core's implementation proposal for [interface issue #2](https://github.com/devincombs2007-rgb/SAIGE/issues/2). The owner reviews these decisions through the core PRs. The first integration target is a Windows x64 executable that draws one moving 3D object, accepts movement input, survives resize and minimize, and releases the device before closing the window. Core supplies the host and movement. Claude supplies the renderer, shader and geometry.

## First-frame contracts

| Decision | Agreement for the first integration |
| --- | --- |
| D-01 — toolchains | C++20. The native Windows target is VS 2022 x64 with a current Windows SDK. Linux hosts the portable core, tools and tests. |
| D-02 — build | The desktop project and presets require CMake 3.21 or newer. Link GPU adapters with the application. Consume `saige::core` and, on Windows, `saige::desktop`. Give graphics targets their own namespaced aliases. Keep Vulkan optional. |
| D-03 — dependencies | The core builds with the standard library and host SDK. For the first Vulkan target, Claude can use an explicitly enabled SDK lookup. When adding a package, pin its version or vcpkg baseline and document the license and build inputs in the graphics PR. |
| D-04 — modules | Claude owns `engine/rhi*`, `engine/render`, `engine/gfx_assets`, `shaders`, `tools/shaderc`, their tests, and `docs/rendering`. Core owns world, platform and application integration. Core headers stay usable in a headless build. |
| D-05 — conventions | Follow existing PascalCase types and snake_case methods. Exceptions and RTTI retain compiler defaults. Validate expected input errors at public entry points and handle startup exceptions at the executable boundary. Keep local graphics errors typed where useful. |
| D-11 — diagnostics | For the first backend, write diagnostics and validation messages to standard error or clog with a graphics category. Reserve stdout for CLI results. Use ordinary debug assertions for internal invariants and explicit errors for recoverable failures. Profiling can be added when there is a measured workload. |
| D-12 — platform | The borrowed Win32 handle pair, versioned surface state, key state and lifecycle are defined below. Vulkan surface and swapchain creation belong to Claude. |
| D-17 — tests | Register tests with CTest. The existing Linux and Windows jobs can run the core, null-backend and tool suites without GPU access. Add a separate explicitly configured graphics job when CPU Vulkan tests exist. Use validation output plus a rendered-image assertion for graphics acceptance. |
| D-18 — startup/shutdown | Construct platform, world and game, then frame sink. Pump via run(). Release GPU work, swapchain, surface and sink before the platform destroys its HWND. Match this order on initialization failures through RAII. |

## Desktop integration

The first native adapter targets Windows 10 version 1703 or later. Create and access the platform on the UI thread. A renderer implementing `IFrameSink` can take an `IDesktopPlatform const&` in its constructor. In a backend-specific source file, cast the Win32 `display` to HINSTANCE and `window` to HWND. Both are borrowed. The platform owns event dispatch and the window procedure.

Read `platform.surface()` after each message pump, for example at the start of submit(). Width and height are client-area pixels. Track the revision across frames, including DPI changes. Defer presentation at zero extent or while minimized. The next nonzero extent permits swapchain recreation. A quit result stops further frame submission. A normal close or Escape leaves the HWND valid until platform destruction.

Input state reflects held keys after the message pump and clears on loss of focus. WASD and arrows control horizontal motion. The runtime calls the game only on fixed ticks. A `FrameView` carries current world positions and entity identities. The renderer may choose its own camera and procedural cube for the first frame. Copy any instance data retained after submit(). An empty world is valid.

## Decisions for later consumers

| Decision | Agreement |
| --- | --- |
| D-06 — coordinates | Use the owner-approved right-handed, Z-up world with one unit per meter. Core accepts +X forward and +Y left as recommended in the [coordinate standard](https://github.com/devincombs2007-rgb/SAIGE/blob/2f0863a/docs/standards/coordinate-system.md). The importer converts source coordinates at the boundary. |
| D-07 — math | Use Core's Vec3 for positions. Agree on explicit column-vector, column-major matrix layouts when first needed. Pack GPU data in the backend and test its size and alignment there. |
| D-08 — threading | Initial extraction and submit run synchronously on the host thread. The backend may overlap GPU work if it owns all retained data and synchronizes resource lifetimes. |
| D-09 — jobs | Serial execution is supported throughout 0.1. Add shared scheduling when the first parallel workload has tests and a profile. |
| D-10 — memory | Standard RAII containers and allocators for CPU data. The backend owns device allocations. |
| D-13 — files | Use explicit paths and synchronous reads for the first sample. Put asset loading behind a consumer interface before introducing streaming or watching. |
| D-14 — assets | Claude owns graphics import, cooking and GPU loading. Core will own persistent asset identity and registry. Define serialized IDs and format versions together before the first reusable cooked asset. |
| D-15 — world integration | Core owns gameplay components and frame extraction. Map the existing read-only positions to one primitive in the backend. Propose new fields for rotation, mesh and material references through a shared-header PR with tests. |
| D-16 — settings | Use a local typed graphics configuration at application startup. Add engine-wide live settings when a consumer requires them. |

D-19 follows the owner’s animation decision. Core owns animation interfaces and engine integration. Claude owns graphics import, skeletal mesh rendering and GPU skinning. Agree on playback, blending, retargeting and IK ownership before implementing that subsystem. The first procedural cube has no skeleton dependency.

## Review and acceptance

1. Review the core foundation relative to main, then the desktop integration relative to the foundation. The owner approves major merges.
2. Claude can implement the first-frame consumer under [RHI issue #3](https://github.com/devincombs2007-rgb/SAIGE/issues/3). Connect it to the desktop sample in a coordinated integration PR. Share a Windows run command and capture of the moving object. Exercise resize, minimize/restore, WASD, Escape and the title-bar close button.
3. Use [shader issue #4](https://github.com/devincombs2007-rgb/SAIGE/issues/4) and [asset issue #5](https://github.com/devincombs2007-rgb/SAIGE/issues/5) for their respective tooling. Record what is required to reproduce each output.
