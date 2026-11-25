# Windows bootstrap and qualification

Two PowerShell scripts for a Windows x64 gaming PC. **Neither has been run on Windows yet** (they were written on Linux, where PowerShell is not available, so they are not even syntax-checked by a machine). The first run
on the owner's machine is their first test; please report problems in issue #43.

* `bootstrap.ps1`: read-only by default; `-Install` fetches missing Git, CMake, Ninja, Python, the Vulkan SDK and the Visual Studio 2022 C++ build tools through winget. It never installs a GPU driver.
* `qualify.ps1`: configures, builds and tests per `docs/windows-renderer-integration.md`, runs `saige_desktop.exe --require-rendered --validate`, classifies every Vulkan adapter as software or physical,
  and writes `qualification\qualification.json` with `qualifies_as_physical_gpu_evidence` (true only with a physical adapter and every step passing). It reports nothing about frame rate.

The interactive checks (resize, minimize and restore, input, clean close) still need a person at the window.
