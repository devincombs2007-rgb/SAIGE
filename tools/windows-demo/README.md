# SAIGE 0.2 Windows x64 scene demonstration

Extract the entire GitHub Actions archive into one folder, then run Launch.cmd.
Keep the scene directory next to the executable. Use WASD to move, the up and
down arrows to rise or descend, right-drag to look, and Escape to close.

The executable targets 64-bit Windows and requires a Vulkan 1.3 loader and a
compatible Vulkan driver. It also requires the Microsoft Visual C++ x64 runtime
if your system does not already have it. The archive ships no GPU driver,
software Vulkan runtime, or validation layer.

For a 30-frame smoke run and a diagnostics log, open PowerShell in the extracted
folder and run:

```powershell
.\Launch.cmd --frames 30 *> diagnostics-local.txt
$LASTEXITCODE
```

The log includes the adapter name, software-device flag, rendered frame count,
completed scene-pass timing where available, resource counts, and memory data.
See renderer-diagnostics.md for field definitions. The evidence directory
contains hosted Windows software Vulkan results and the source commit used for
the build. Physical Windows GPU qualification is tracked in GitHub issue #9.
SHA256SUMS.txt records hashes relative to the archive root.

This demonstration uses a small original glTF scene. The renderer samples base
color textures, and glTF Blend materials draw opaque. Reopening in the test
starts a new native process. Separate renderer tests cover failed scene
replacement and retirement of resources that are still in flight.
