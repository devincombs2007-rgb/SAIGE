# Dielectric energy behavior and the environment BRDF table

Two questions from the 0.3 review: does the renderer's dielectric BRDF behave as the glTF specification defines, and can the
runtime-generated environment BRDF table become a precomputed, versioned asset without losing accuracy?

## 1. Dielectric BRDF energy: faithful to glTF, and not energy conserving

### The renderer implements the specification exactly

The glTF 2.0 specification (appendix B, "Metal BRDF and Dielectric BRDF", read from the Khronos repository text) defines:

    fresnel_w = (1 - abs(VdotH))^5
    diffuse_brdf = (1 / pi) * baseColor
    specular_brdf = D(roughness^2) * G(roughness^2) / (4 * abs(VdotN) * abs(LdotN))
    dielectric_fresnel = 0.04 + (1 - 0.04) * fresnel_w
    dielectric_brdf = mix(diffuse_brdf, specular_brdf, dielectric_fresnel)
    metal_brdf = (baseColor + (1 - baseColor) * fresnel_w) * specular_brdf
    material = mix(dielectric_brdf, metal_brdf, metallic)

`shaders/scene/pbr.hlsli` (`evaluate_brdf`) and the CPU reference `brdf_reference.hpp` implement this, with GGX D, the
height-correlated Smith visibility, and the roughness clamp of 0.045 as the only addition (to keep a mirror finite; the
specification leaves the limit to the implementation). GPU output is compared against the reference in `test_pbr.cpp`.

### The specification itself says this coupling is not energy conserving

The specification's discussion ("Coupling Diffuse and Specular Reflection") states that `fresnel_mix` "is simple and cheap to
compute, it is not very accurate and breaks a fundamental property that a physically based BRDF must fulfill, energy
conservation", and lists fixes (Burley 2012, Jakob et al. 2014, Kulla and Conty 2017). It also notes that the sample viewer
uses approximations that break energy conservation and reciprocity. So the overshoot is a property of the specified model, not
a renderer bug.

### How large it is (independent verification)

`tools/analysis/dielectric_energy.py` was written from the specification text, without reading renderer code, so it can
disagree with it. White furnace test for a white dielectric (`numpy`, deterministic; its integrator agrees with a brute-force
3000x3000 quadrature to four decimals at four test points):

| roughness | n.v 0.05 | 0.1 | 0.2 | 0.4 | 0.7 | 1.0 |
|---|---|---|---|---|---|---|
| 0.10 | 1.6800 | 1.5348 | 1.2974 | 1.0675 | 1.0006 | 0.9999 |
| 0.20 | 1.4839 | 1.4278 | 1.2694 | 1.0667 | 1.0009 | 0.9998 |
| 0.30 | 1.3263 | 1.2837 | 1.1983 | 1.0599 | 1.0013 | 0.9996 |
| 0.50 | 1.1317 | 1.1097 | 1.0736 | 1.0252 | 0.9981 | 0.9966 |
| 0.70 | 1.0473 | 1.0349 | 1.0175 | 0.9984 | 0.9887 | 0.9879 |
| 1.00 | 1.0013 | 0.9932 | 0.9844 | 0.9776 | 0.9743 | 0.9722 |

At normal and moderately oblique views (n.v of 0.7 and above) the albedo lies between 0.972 and 1.002 (it loses up to 2.8 percent at
high roughness, the single-scattering energy loss, and never gains more than 0.2 percent). The excess grows toward
grazing views of smooth surfaces: 33 percent at n.v 0.05 and roughness 0.3, 68 percent at roughness 0.1. It exceeds 1.02 only for
n.v of 0.4 or less. This corrects an earlier rough figure ("up to about 30 percent at roughness 0.3"): that was the n.v 0.05
value, and smoother surfaces are worse. Visible effect: a thin bright rim at silhouettes of smooth dielectrics under uniform
light, in a single-light renderer a modest brightening of grazing highlights. It is bounded by the display clip in the LDR path
and passes through the tone map in the HDR path.

### Faithful compatibility versus optional improvement

| | Definition | Furnace albedo | Cost |
|---|---|---|---|
| **glTF faithful (current default)** | specification formulas | up to 1.68 at grazing, smooth | none |
| Burley-style diffuse `(1-F(n.l))(1-F(n.v))` | named in the specification | at most 0.954, but white albedo darkens to 0.26 at grazing | trivial |
| Diffuse weighted by `1 - E_spec(n.v)`, Kulla-Conty coupling | named in the specification; needs the Fresnel-weighted specular albedo | 1.0 for a white base by construction (single scattering) | one lookup in the table we already ship (`E_spec = f0 * scale + bias` with f0 = 0.04) |

Decision (owner, 0.3 review): keep the glTF-faithful model as the default, because content authored and previewed in other glTF
viewers expects it. An optional energy-conserving model is approved for M5, **explicitly selectable, independently tested and
documented**, never silently changing existing material behavior. The table makes the Kulla-Conty style coupling nearly free
(one lookup), and it matters most under bright, uniform environment light. Requirements when it is built: its own CPU
reference (furnace test bound of 1 for a white base, to the single-scattering accuracy stated above), its own GPU-versus-reference
tests, a selector that defaults to the faithful path, and documentation of the difference. No code change yet.

## 2. The environment BRDF table as a precomputed, versioned asset

Measured with the real integrator (`-O2`):

| table | build time | worst error, 400 random points |
|---|---|---|
| 32x32, 48 steps | 16 ms | 0.029 |
| 32x32, 128 steps (what M2 shipped) | 114 ms (233 ms in a Debug build) | 0.020 |
| 32x32, 512 steps | 1.8 s | 0.020 |
| 64x64, 128 steps | 464 ms | 0.012 |
| 64x64, 512 steps (chosen) | 7 s once, at generation | 0.005 |

Interpolation error is set by the table size, not the step count, so a precomputed table can afford both more texels and more
steps. Against an independent integral the embedded 64x64 table measures 0.0023 worst at texels and 0.0012 at random points
between them.

Implemented in PR #55 (stacked on M2): an embedded `src/generated/env_brdf_table.inc` (16 KB, version, size, steps, FNV-1a
checksum), `saige_envbrdf_gen generate|verify`, and the CTest `saige_env_brdf_table_up_to_date`, which checks metadata, the
checksum, the physical range of every texel and recomputes 68 texels from the code to within 2 half ulps. Mutations (edited
texel, changed clamp, changed Fresnel exponent) each fail it. Embedded rather than a loaded file: no path resolution or
packaging failure mode, 16 KB. Regenerating requires bumping `kEnvBrdfTableVersion`.

Reproducibility: the 2-half-ulp tolerance held on hosted Linux, ASan and Windows MSVC CI for #55. The owner approved keeping the
embedded 64x64 table, provided accuracy and cross-platform reproducibility stay verified by that test.
