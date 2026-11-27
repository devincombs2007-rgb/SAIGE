# ModelRenderer local lights: validation evidence (2026-11-27)

What this is: the forward local-light path of `ModelRenderer` (`ModelRenderSettings::local_light_capacity`, permutation `SAIGE_LOCAL_LIGHTS`, `shaders/scene/model_lit.hlsl`), see `docs/rendering/local-lights.md` (fourth step).

Adapter class: **software Vulkan (llvmpipe/lavapipe), not a GPU.** Everything here is correctness evidence. Nothing here is a performance, scalability or physical-GPU statement.

## Images (1920 x 1080, written by the capture case of `engine/rhi_vulkan/tests/test_model_local_lights.cpp`)

| File | What it shows |
| --- | --- |
| `model_local_lights_unlit_1080p.png` | A dusk scene (grey floor, five primitives) with local lights published as empty: the baseline. |
| `model_local_lights_lit_1080p.png` | The same frame with two point lights (warm, blue) and a spot light published through `SceneFrame::local_lights`. |
| `model_local_lights_difference_x8_1080p.png` | The absolute difference of the two, times 8. |

Reproduce: `SAIGE_CAPTURE_DIR=<dir> saige_rhi_vulkan_tests "Local lights in the model renderer"` (PPM files; convert with any tool).

Visible limits in the picture: the lights cast no shadows (the primitives do not occlude them); the far wall is outside every light's radius and stays dark; there is no culling.

## What the tests assert (all on a validated device with synchronization validation; zero validation errors)

1. Floor through the renderer, lit minus unlit, against the CPU oracle: 163 probes, 118 inside a light's reach, 45 beyond all lights (must be unchanged by even one quantum); worst per-channel error R 0.0033, G 0.0049, B 0.0032 (linear) against a tolerance of 0.012.
2. Zero lights with the permutation on is byte-identical to capacity 0 (no publication, empty list, a black light, and the frame after a lit frame).
3. Overflow: lights beyond the capacity are dropped, counted, and the image is byte-identical to a renderer whose capacity is that count.
4. Invalid input (non-finite, radius outside (0, 1e5], negative colour or intensity, non-finite cone) and unsupported combinations (capacity above 256, no dynamic lighting, non-MetallicRoughness shading, shadows, skinning, instancing) are refused and change nothing.
5. Nine consecutive frames with different lights (more than frames in flight) each match their own lights (worst error 0.0067), and a frame that publishes nothing draws no light.
6. `ModelRenderer::set_local_lights` validates before writing, refuses a second publication in a frame, and refuses a renderer without the capacity.

Mutation: 10 of 10 mutants caught (loop removed, count ignored, count replaced by capacity, wrong record index, off-by-one loop, frame slot always 0, dropped lights not counted, lights written beyond the capacity, record validation removed, unpublished frame keeps the previous slot). A first version of the tests missed two of them; the direct `set_local_lights` test and a `SceneRenderer` change (an unpublished frame relies on the draw-time completion) were added in response.

Regression: the five other model permutations regenerate to byte-identical SPIR-V (only the source-hash line of the generated headers changes); the full local CTest run is 175 tests, 0 failures, 9 skipped for missing optional dependencies (Jolt and licensed assets, present in hosted CI).

## Composition with sun shadows and IBL (fifth step, same day)

| File | What it shows |
| --- | --- |
| `model_local_lights_shadows_ibl_unlit_1080p.png` | An evening scene with a low sun (long sun shadows), a sky-gradient environment with a warm band (IBL), three blocks and three spheres: chrome (metal, roughness 0.08), brushed gold (metal, 0.35) and red lacquer (dielectric, 0.15). Local lights published as empty. |
| `model_local_lights_shadows_ibl_lit_1080p.png` | The same frame with two point lights (warm, blue) and a spot light. |
| `model_local_lights_shadows_ibl_difference_x8_1080p.png` | Their absolute difference, times 8. |

Looked at by the author (lit and unlit): the point lights and the spot put specular highlights on all three spheres and warm, blue and spot pools on the floor; the chrome sphere reflects the environment (its warm band on the right); the sun shadows of the blocks and spheres are long. Visible limits: **the sun-shadow edges are stair-stepped** at this map resolution (1024, 40 m; the known thin-shadow aliasing, S4-009); **local lights cast no shadows**; **reflections are the environment only**: the chrome sphere does not reflect the floor or the blocks (scene reflections are not implemented). The difference image was not looked at. Software Vulkan only.

Reproduce: `SAIGE_CAPTURE_DIR=<dir> saige_rhi_vulkan_tests "stills of local lights with sun shadows"`.
