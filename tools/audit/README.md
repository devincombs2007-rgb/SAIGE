# Toolchain and dependency audit

```
python3 tools/audit/audit.py                                  # table on stdout; exit 1 only when a required tool is below its minimum or a pin is missing
python3 tools/audit/audit.py --json audit.json --markdown audit.md
```

Offline and read-only. It reports, against `policy.json`:

* tool versions (CMake, Ninja, g++, clang++, glslang, spirv-val, dxc, vulkaninfo, Blender, ffmpeg, Python): `min` fails, `recommended` warns, `optional` tools may be absent;
* whether each compiler really builds C++20 (required) and C++23 (probed only: moving the project to C++23 is a separate decision made on this evidence);
* every `FetchContent_Declare` in the tree and whether it is pinned by content hash or a full commit id (a tag alone is a warning);
* the Vulkan adapters, each classified as software or physical, so nothing measured on a software rasterizer is mistaken for GPU evidence;
* the newest reviewed version of a dependency the project is behind on (recorded by hand in `policy.json` when it is reviewed; the audit never touches the network).

Packaging candidates that are evaluated but not adopted (Jolt, Bullet, miniaudio) are listed in the policy with the reason; adopting one needs the owner's approval and a license, security and maintenance review.

Tests: `python3 -m unittest discover -s tools/audit` (also CTest `audit_tools_python`).
