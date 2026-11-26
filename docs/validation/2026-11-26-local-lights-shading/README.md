# Local light shading function: GPU against the CPU reference

* **Engine commit.** The head of `claude/fidelity-dev` containing this file; the exact 40-character SHA is in the issue #43 checkpoint that accompanies the push.
* **Technology.** Point and spot lights, step 2 of `docs/rendering/local-lights.md`: the HLSL shading function `shaders/scene/local_lights.hlsli`, run by a test-only compute shader, checked against `saige::render_reference` (lights) and the BRDF oracle (`gfx_test` `brdf_reference.hpp`). It is **not yet wired into the model renderer**: this is a diagnostic of the function, not a lit scene.
* **Backend and conditions.** Software Vulkan (Mesa lavapipe/llvmpipe), validation layers and synchronization validation on, 0 validation errors. Not a physical GPU. No performance claim.

## The image

`local_lights_floor_gpu_cpu_diff.jpg`, three 256 x 256 panels left to right: the GPU result, the CPU reference, and their difference with a gain of 8. A 24 m x 24 m floor (dielectric, base colour 0.72/0.70/0.66, roughness 0.55) seen from (0, -14, 12), lit by a warm point light (500 cd, radius 14 m) at the left, a cool point light (400 cd, radius 10 m) at the right, and a white spot (30,000 cd, radius 20 m, inner/outer half angles 0.22/0.45 rad) aimed at the floor from above. Reinhard tone mapping and sRGB encoding for display only; the numbers are compared before tone mapping.

## Looked at, by whom

I (Claude Code) opened the image: the two point lights are smooth pools that reach zero at their radii (no visible edge), the spot is an ellipse (oblique incidence) with a soft edge from the cone's t^2 profile, and the difference panel is black. The director and the owner have not inspected it. A black difference panel at gain 8 means the 8-bit images are identical; it does not mean the lighting model is *right*, only that the shader equals the reference, which is the stated contract.

## Measured

* 700 random probes x 28 lights: worst error 0.058 of the tolerance (0.2% relative plus 0.002), 1,686 non-zero channels, 0 out of tolerance.
* The 256 x 256 floor: 65,536 pixels, 55,893 lit, 0 channels out of tolerance.
* Exact cases: zero at and beyond the radius, zero outside the spot's outer cone, zero for a surface facing away, finite and equal to the reference on the light and at 5 cm (distance clamped at 1 cm), nothing for a light of radius 0.
* Mutants: 11 of 13 shader mutants caught (radius window removed or not squared, linear cone, wrong minimum distance, 1/d falloff, no n.l, flipped cone sign, colour ignored, cone scale ignored, Lambert instead of the BRDF, un-normalised direction). The 2 survivors are equivalent on valid input (see `docs/rendering/local-lights.md`).

## Known limitations

* Not in the renderer: no light buffer in the frame, no clustered culling, no shadows from local lights, no area lights or IES profiles. Emissive materials are still not lights.
* Software Vulkan only; nothing says how many lights a frame can afford.
* The tolerance is a float comparison between a GPU function and a double-precision reference with a float BRDF oracle: it shows agreement, not physical accuracy of the model.

## Reproduce

```
cmake -S . -B build -DSAIGE_ENABLE_VULKAN=ON -DSAIGE_VULKAN_FETCH_HEADERS=ON -DBUILD_TESTING=ON
cmake --build build --target saige_rhi_vulkan_tests
SAIGE_CAPTURE_DIR=captures SAIGE_REQUIRE_VALIDATION=1 build/engine/rhi_vulkan/saige_rhi_vulkan_tests "Local lights"
```
