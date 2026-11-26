# SSR integration contract: what SAIGE supplies today, and a versioned plan

Source: `validation/ssr/upstream/docs/SSR_ENGINE_INTEGRATION_CONTRACT.md` (contract version 2, `kSsrContractVersion`). SSR is Gemini's; the renderer-side interfaces and the eventual integration are Claude's. **Not integrated; no production code changes; integration waits for validation closure and the director's explicit approval.** Validation results: `validation/ssr/README.md`.

## Inputs the contract requires versus what SAIGE has (read from the code and docs at this commit)
| Input | Contract | SAIGE today | Gap |
| --- | --- | --- | --- |
| HDR scene colour | RGBA16F or RGBA32F linear, at render resolution, jittered | The forward renderer draws into an HDR target (RGBA16F), then bloom, tone-map. Always output resolution | A render-resolution target and a scale setting (dynamic resolution) |
| Depth | D32 or R32F, reversed-Z flag | D32 depth exists, sampleable since the AO pass (`ambient-occlusion.md`). Reverse-Z (`perspective_reverse_z`, `CompareOp::Greater`), which is the contract's default | None for the convention; SSR's absolute 0.05 disocclusion threshold is the problem (finding A2): far surfaces sit near 0 |
| Per-pixel motion vectors | RG16F/RG32F in render pixels, `prev - curr` | **None**: `fidelity-plan.md` records "no taa, jitter or velocity buffer in engine/render"; `FrameView` carries the current frame only (`material-architecture.md`) | A velocity pass; previous-frame world matrix per instance, previous camera pose (interface change with Core, raised on #17); skinned and animated draws need the previous joint palette and positions |
| Camera jitter | Halton, [-0.5, 0.5] px | CPU reference only (`render_reference/taa.hpp`: Halton(2,3), `jitter_ndc`); the renderer applies none | Projection jitter in the renderer (renderer-internal) |
| Exposure | `pre_exposure` multiplier, history rescaled by the ratio | A scalar `exposure` in the atmosphere pass; no auto or pre-exposure | A single frame exposure value shared by the scene, SSR and tone-map |
| Reactive mask | optional R8 | none | A particle and emissive-VFX mask pass |
| Transparency mask | optional R8 | Sorted alpha blending exists as a branch (`alpha-blending.md`); it writes no mask | A mask written by the blend pass |
| Temporal history | persistent RGBA16F, R32F depth, R16F confidence at output resolution | none | Per-view history ownership and lifetime (resize, device loss) in the renderer |
| Dynamic resolution | any render size not above output | none (quality presets exist for shadows only) | A controller and a resize path that keeps history (SSR already keeps history when only the render size changes) |
| Camera-cut signal | `camera_cut` | none as an engine signal (the Showcase 04 camera director has its own cut flags) | A `FrameView` flag set by teleports, scene replacement and chunk streaming switches |

## Versioned plan (each step independently testable, CPU reference first, software Vulkan correctness only until a physical GPU is available, #9)
* **R0 (now):** this assessment; Gemini fixes A1 and A2 and reruns `ssr_*` ctest; the validation branch stays isolated.
* **R1 `FrameView` v2** (interface proposal, Core consulted on #17): `prev_view_projection`, `prev_world` per instance, `jitter`, `exposure`, `camera_cut`, render size. Additive, no behaviour change; architecture document records lifetime and threading.
* **R2 velocity target and jitter:** jittered projection plus a velocity pass for static and rigid draws; CPU reference check against `render_reference::taa`; then skinned and instanced draws. Acceptance: per-pixel velocity equals the analytic reprojection on a known camera move; capacity 0 or jitter off leaves goldens byte-identical.
* **R3 render-resolution scene and history ownership:** the renderer renders at `compute_render_resolution` and owns SSR's three history images; validation clean across frames in flight.
* **R4 SSR pass behind a capability gate and a quality mode** (off by default, no change to 0.2 output): compute pass via the existing RHI compute path; the harness's Vulkan code is replaced by RHI calls; the validation suite's checks (gates, directed sequences, mutants) move to the RHI path unchanged.
* **R5 reactive and transparency masks;** **R6 dynamic resolution controller.**
* Gate to start R4: validation closed (A1, A2 resolved or accepted), a physical-GPU check scheduled, and director approval.
