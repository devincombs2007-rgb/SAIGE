# Independent validation of Gemini's SSR Build 002.1 (SAIGE Super Resolution)

Branch `claude/ssr-validation` (from `claude/fidelity-dev`). **Nothing here is linked into the production renderer**, nothing is merged, and SSR is
**not** declared production-ready. Evidence, numbers and images: [`docs/validation/2026-11-26-ssr-validation/`](../../docs/validation/2026-11-26-ssr-validation/README.md).
Integration inputs and a versioned plan: [`docs/rendering/ssr-integration-plan.md`](../../docs/rendering/ssr-integration-plan.md).

## Layout

| Path | What |
| --- | --- |
| `upstream/` | The delivered package, **verbatim** (zip SHA-256 `2e10244467ebb38ad0e277ea8949e7ec81a35b0e7de63de04c146406aaf509a1`; all 24 files match `MANIFEST.json`; the three visuals are recorded by hash in `upstream/visuals/SHA256SUMS`, not committed). `patches/gemini_ssr.patch` was applied to an empty directory first: it only adds the seven source files, byte-identical to the delivered tree, and touches no SAIGE file. |
| `harness/` | A **modified copy** of Gemini's Vulkan harness. Every change is marked `VALIDATION CHANGE (SAIGE)`; `diff -u upstream/gpu harness` is the complete list. The algorithm (`upstream/shaders`, `upstream/source`) is untouched. |
| `tests/ssr_validation_test.cpp` | The validation suite (negative, directed, strict-gate, lifetime tests) and the mutant runner. |
| `tools/` | `build_ssr_shaders.py` (compile and `spirv-val` every binary, build the 14 shader mutants), `check_spirv_interface.py`, `ssr_visual_dump_main.inc` (visual and measurement tool, appended to a copy of Gemini's own test so the scenes, TAA-U baseline and metrics are the delivered ones), `compose_ssr_evidence.py`. |
| `CMakeLists.txt` | 20 ctest tests on the existing hosted lavapipe lane: `ssr_validation`, `ssr_upstream_suite`, `ssr_spirv_interface`, 14 `ssr_mutant_*`, 3 `ssr_harness_mutant_*`. Skipped silently where libvulkan, glslang, spirv-tools or Python are missing (ASan and Windows lanes). |

## CI contract (director review 7202180515)
* The hosted lavapipe lane configures with `-DSAIGE_REQUIRE_SSR_VALIDATION=ON`: a missing libvulkan, glslang, spirv-tools or Python is a **configure error**, not a silent skip, and its test step asserts that exactly 20 `ssr_*` tests are registered and sets `SAIGE_REQUIRE_SSR=1`, which turns a missing Vulkan device or validation layer into a failure (exit 1) instead of a skip (exit 77). Verified locally: the required configure fails with the prerequisite missing, and the unrequired one prints `SSR validation: NOT BUILT, no SSR test coverage on this configuration (...)`.
* **Unsupported lanes, stated:** the ASan lane (no libvulkan or Vulkan device) and the Windows lane (the harness is Linux-only) do **not** build or run the SSR tests and have **no SSR coverage**; their green status says nothing about SSR.
* `ssr_validation` must print `ALL PASSED (N checks executed)` with N of at least 100 (204 now); a run that executed nothing cannot pass. `WILL_FAIL` is gone: each shader mutant must be caught by the gate named in `CMakeLists.txt` (`scaled`, `tight` or `directed`, printed as `gate=FAIL ... -> DETECTED`), and each harness mutant by its own reason (`!r.parity_passed` check, the layer's `InvalidImageLayout` error, the injected-error check). A crash or an unrelated nonzero exit is not a detection.

## Defects found (and what was done)

### Harness (Gemini's `gpu/ssr_vulkan_harness.*`): corrected in the validation copy
1. **An empty CPU reference counted as numerical parity success** (the director's finding). Now a missing, empty, mis-sized or non-finite reference is `parity_evaluated = false, parity_passed = false` with a status that says why; a wrong but well-formed reference fails. Tests: empty, short, long, NaN, infinity, all-zero and a bright wrong reference; mutant `EMPTY_REFERENCE_PASSES` (the delivered behaviour) is caught.
2. **The delivered GPU parity test fails its own validation check on its first run.** The history inputs were transitioned by checking only `hist_color`'s layout; `hist_depth` and `hist_conf` stayed `GENERAL` while the descriptor declared `SHADER_READ_ONLY_OPTIMAL` (`UNASSIGNED-CoreValidation-DrawState-InvalidImageLayout` at `vkQueueSubmit`, second frame; `tests/ssr_test.cpp:550` fails). Fixed (each history input is checked); mutant `LAYOUT_COLOR_ONLY` is caught by the validation layer.
3. The harness did not compile against the distribution's Vulkan 1.3.275 headers (`VK_API_VERSION_1_4`); it needs SAIGE's pinned 1.4.363 headers (as every SAIGE lane has). The corrected copy negotiates `min(loader, 1.4)` and requires 1.3.
4. `validation_clean` was `true` when the layer was off, and synchronization validation was never enabled. Now `validation_clean` needs the layer active and zero messages, `VK_EXT_validation_features` synchronization validation is requested (and reported as active or not), and a test injects an invalid call to prove messages reach the report (mutant `VALIDATION_ALWAYS_CLEAN` caught).
5. Unchecked Vulkan results (about twenty creation, mapping, submit and wait calls); a failed allocation left null handles and a poisoned allocation record. Now checked, allocations dropped and rebuilt after any failure, bounded fence wait (10 s), size limits.
6. No format-feature checks: the shaders sample RGBA32F, R32F and RG32F with a linear sampler (an **optional** format feature), store to RGBA16F, R32F, R16F, and the SPIR-V declares `StorageImageExtendedFormats` (the device feature `shaderStorageImageExtendedFormats` was not enabled). Now checked and enabled before anything is created. llvmpipe supports all of it, so the rejection paths are **not exercised on a device that lacks them**.
7. Silent fallback to physical device 0 when the preferred kind (software vs hardware) did not exist; `strict_device_selection` added. Timestamps used without checking `timestampValidBits`; now checked and masked.
8. No SPIR-V magic check: a zero-filled file made the validation layer try to allocate 16 GiB (found by the ASan lifetime test). Refused before `vkCreateShaderModule`.
9. The delivered test printed `ALL 4 SUITES PASSED` when no Vulkan device existed (the GPU suite was skipped silently), and included the harness header through `../gpu/`, which silently selects the wrong `VulkanParityReport` layout when another copy of the header is on the include path (ODR trap, found as an ASan stack overflow). The CMake build redirects the include; `ssr_validation` exits 77 (skipped) without a device.

### The delivered parity gate has little power
On a stress sequence only **2 of 14** shader mutants failed the contract's `35 dB / 0.025 MAE` gate (the tolerance covers the GPU's bilinear sampling versus the CPU's Catmull-Rom, so it hides real errors; it is also measured in tone-mapped sRGB, which saturates above luminance about 10 and cannot see HDR range errors). Added: region-of-interest MAE, a tight native-resolution zero-jitter sequence (no kernel difference: first and cut frames agree to 79 dB), and three directed sequences (disocclusion, confidence, depth dilation). **14 of 14 mutants are now caught** (`exposure_ignored`, `jitter_sign`, `cut_ignored`, `mv_sign`, `depth_test_flipped`, `no_disocclusion`, `no_history_clamp`, `alpha_raised`, `transparency_ignored`, `confidence_constant`, `depth_history_zero`, `decompress_floor`, `sharpen_off`, `sharpen_north_south`); the gates are measured baselines with margin (constants and baselines in `tests/ssr_validation_test.cpp`). The gates were calibrated on llvmpipe; another device may need re-measuring.

### Algorithm findings, returned to Gemini through the director (no competing implementation, no shader change)
* **A1 HDR ceiling.** After the temporal path a highlight cannot exceed luminance about 1000: `hdr_decompress` floors its divisor at `1e-3` (the CPU reference caps at `0.999`). Measured through the GPU path: input luminance 4e3, 4e4, 4e5 give an output peak of 999.7, 999.9, 999.9. The first or cut frame has no such cap (65504, the RGBA16F maximum), so a sun or specular highlight pops down by orders of magnitude on the frame after a cut. The CPU reference has the same ceiling, so CPU-GPU parity cannot see it.
* **A2 Disocclusion in raw depth.** `abs(center_depth - prev_depth) > 0.05` on reversed-Z perspective depth: far surfaces sit near 0 and can never differ by 0.05, so far-field disocclusion is never detected (analysis, with the directed sequence showing the near-field path works); the previous depth is also read with a linear filter, which blends depths across silhouettes.
* **A3 The 35 dB parity tolerance is a statement about band-limited content.** Same 9-frame sequence, GPU vs CPU worst frame: edge softness 6 px 38.99 dB, 2 px 29.68 dB, hard edges 28.53 dB. On Gemini's own `rapid_camera_pan_and_cut` scene at 640x360 the worst GPU-vs-CPU frame is **34.74 dB, below the contract's 35 dB** (the delivered test only runs 160x90).
* **A4 Detail.** On the procedural foliage and wires scene the GPU path's SSIM (0.6245) is below bilinear (0.6521) and equal to the delivered TAA-U baseline (0.6230); PSNR is slightly higher. In the 1:1 crop the thin wires are continuous (bilinear breaks them into dashes) but the leaf pattern is blurred as in TAA-U.
* **A5 The scenes are procedural patterns** (hatch and grid lines, a striped rectangle, a bright disc), not rendered SAIGE content, and the TAA-U baseline is Gemini's own simple implementation. No statement about vehicles, glass, DLSS or FSR follows. The delivered "1080p" stills are 2x2 mosaics of 960x540 panels.

## Not verified
Physical-GPU behaviour and performance (everything ran on llvmpipe; its timestamps are valid but **not** performance evidence); the FP16 path on any device but llvmpipe; Windows; the rejection paths of the format and feature checks; real renderer inputs (motion vectors, jitter, exposure) which SAIGE does not produce yet.

## Reproduce
```
cmake -S . -B build -G Ninja -DSAIGE_ENABLE_VULKAN=ON -DSAIGE_VULKAN_FETCH_HEADERS=ON      # needs libvulkan-dev, glslang-tools, spirv-tools, vulkan-validationlayers
cmake --build build --target saige_ssr_validation_shaders ssr_validation_test ssr_upstream_suite
ctest --test-dir build -R "^ssr_"                                  # 20 tests, about 30 s
build/validation/ssr/ssr_validation_test --findings                # prints the measured findings
build/validation/ssr/ssr_visual_dump out/ bench|stills|clip        # then tools/compose_ssr_evidence.py
python3 -I validation/ssr/tools/check_spirv_interface.py spirv-dis build/validation/ssr/shaders
```
