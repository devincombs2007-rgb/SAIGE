# Local lights: point and spot (design, CPU reference done, GPU not started)

Status (2026-11-26): `saige::render_reference` has the **CPU reference** (`engine/render_reference/include/saige/render_reference/lights.hpp`, 16 tests, 18 of 19 compiled mutants caught, the survivor equivalent). No shader, no light
buffer, no culling pass exists yet. Showcase 04 had no local lights at all: windows, lamps, headlights and sirens were emissive surfaces with bloom, and they lit nothing (defect S4-015, `docs/showcase/04-defect-register.md`). **Emissive is not
a light**: an emissive material contributes to no other surface, and nothing in this design reads a material.

## Units and the contract

The engine's convention for the sun (pbr.hlsli): a directional light's `color` is the radiance a white Lambertian surface facing it reflects, i.e. its irradiance is `color * PI`. A local light is described the same way, as the irradiance
it produces on a surface that faces it squarely at 1 m, `intensity`, times an RGB hue. Photometric authoring converts directly: a source of F lumens is `F / (4 PI)` cd (point) or `F / (2 PI (1 - cos outer))` cd (spot, the cone's half angle),
and 1 cd is 1 lux at 1 m, so **intensity in candela is irradiance at 1 m in lux**; exposure maps lux to display exactly as it does for the sun.

* Falloff: inverse square, `E = I / max(d, 1 cm)^2`.
* Influence window: `saturate(1 - (d / R)^4)^2`, exactly zero with zero slope at the light's `radius` R (no visible edge; a light can be culled to a sphere of radius R without changing the picture).
* Spot cone (half angles): `t = saturate((cos a - cos outer) / (cos inner - cos outer))`, attenuation `t^2`.
* The light's contribution to a pixel is `BRDF(l, v) * E * max(n . l, 0)` with the existing BRDF (`pbr.hlsli`, mirrored by `gfx_test_support/brdf_reference.hpp`): the lights module defines sources only, never a second BRDF.

## Why this order (dependency)

1. CPU reference (done): the oracle, with the physics under test (conversions, inverse square, window slope, cone, flux conservation by integration).
2. A light buffer and the shading function in `pbr.hlsli` (`shade_point`, `shade_spot` next to `shade_directional`), compared against the CPU reference on a lavapipe test that renders a plane under each light type.
3. Culling. A scene can have thousands of lights (street lamps, headlights of every car, windows lit as real lights only when near): forward shading of every light at every pixel does not scale. The plan is **clustered forward**: a compute pass
   (the RHI compute pipeline, dispatch and storage buffers now exist, `docs/rendering/rhi-compute-indirect-proposal.md`) bins lights into a 3D grid of view-space clusters (16x9x24), writes per-cluster index lists into a storage buffer, and the model
   pixel shader walks its cluster's list. The GPU result is compared with the CPU culling (a light is in a cluster iff its sphere of radius R meets the cluster's AABB): the same pattern as `InstanceCuller`.
4. Shadows from local lights (spot: one perspective map; point: cube or dual-paraboloid): not before the lights themselves are correct; the budget (a handful of shadowed lights, the rest unshadowed) is a decision for after measurement.
5. Consumers, in this order: street lamps (a spot-like distribution; a photometric profile later), vehicle headlights and brake lights, emergency lights (coloured, animated intensity), interior lights; each a data item in the district (a light is placed by the
   world, so the validator and the editor see it).

## Honest limits

* A reference is not an implementation. Nothing here is rendered yet and no performance claim is made; software Vulkan establishes correctness only, never GPU performance.
* Area lights and IES profiles are not designed: real lamps are not points. Soft highlights from a small sphere light and a lamp's real distribution need them; they come after punctual lights work.
* Light leaking, specular aliasing on tiny highlights and the interaction with the sun's cascade shadows are not addressed.
* Exposure continuity across the day to night transition (S4-025) is a separate defect: a correct night needs lights AND an exposure that adapts; neither alone fixes it.
