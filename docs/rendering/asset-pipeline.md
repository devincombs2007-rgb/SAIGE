# Graphics Asset Pipeline: glTF 2.0 & PBR Materials

**Status:** Proposal. Depends on D-06 (coordinates), D-13 (VFS) and D-14 (asset system ownership).

## 1. Pipeline stages

```
 Source files            Import (tools)              Cook (tools, per platform)        Runtime
 ────────────            ──────────────              ──────────────────────────        ───────
 model.glb        ──►  Source assets:          ──►  Cooked assets:               ──►  GPU resources
 albedo.png            · MeshSource                  · .smesh (quantized, meshlets,     via IAssetLoader
 normal.png            · TextureSource                LODs, GPU-ready layout)          (staging → transfer
 sky.exr               · MaterialSource              · .ktx2 (BCn, full mip chain)      queue → bindless index)
                       · SceneSource (→ Core)        · .smat (template + params)
```

- **Import** turns a third-party format into engine source assets with stable `AssetId`s. It is
  lossless where possible.
- **Cook** turns source assets into platform-optimized, compressed, memory-ready data. Cooking is
  deterministic (the same input always produces byte-identical output), which enables content-hash
  caching and distributed cooking.
- **Load** turns cooked bytes into GPU resources with minimal CPU work: the data is uploaded as-is.

Importers and cookers live in tools and editor builds only. Shipping builds contain only the loaders.

## 2. glTF 2.0 import (parser: fastgltf)

**v1 support (Phase 2):**

| glTF feature | Handling |
|--------------|----------|
| `.gltf` + `.bin`, `.glb`, data URIs | Supported |
| Meshes / primitives | Each primitive becomes a submesh with its own material slot. Triangles only; strips and fans are converted. |
| Attributes | `POSITION`, `NORMAL`, `TANGENT`, `TEXCOORD_0/1`, `COLOR_0`. `JOINTS_0`/`WEIGHTS_0` are kept for later skinning. |
| Node hierarchy | Exported as a `SceneSource` that Core turns into entities/prefabs (D-14/D-15). Static meshes can optionally be flattened. |
| Materials | Metallic-roughness core plus the extensions below |
| Textures / samplers / images | PNG and JPEG embedded or external. `KHR_texture_basisu` (KTX2) passes through. |
| `KHR_texture_transform` | Baked into UVs where it's uniform per material, otherwise stored as a material parameter |
| `KHR_mesh_quantization`, `EXT_meshopt_compression` | Decoded on import |
| `KHR_lights_punctual`, cameras | Exported into the `SceneSource` |
| `KHR_materials_emissive_strength`, `_clearcoat`, `_transmission`, `_ior`, `_specular`, `_sheen`, `_volume` | Mapped to the Standard PBR model (§5) |
| Skins, animations, morph targets | **Rendering owns the import** (owner decision D-19). Skeletons, inverse bind matrices, joint weights, clips and morph targets are imported in Phase 2b. The runtime clip and skeleton formats are agreed with the animation subsystem's interface owner (Core) before implementation. |
| `KHR_draco_mesh_compression` | Deferred. Reject with a clear error message. |

**Coordinate conversion (D-06 approved: right-handed, Z-up, meters):** glTF is right-handed and Y-up,
in meters. The importer rotates +90° about X, `(x, y, z) → (x, −z, y)`, and applies it to positions,
normals, tangents, node transforms, inverse bind matrices and animation tracks. It's a proper rotation,
so winding is preserved. See [docs/standards/coordinate-system.md](../standards/coordinate-system.md).

## 3. Mesh processing (cook)

1. Weld duplicate vertices, then remove degenerate and zero-area triangles.
2. Generate normals if missing (angle-weighted). Generate tangents with **MikkTSpace** when missing,
   as the glTF spec requires.
3. Optimize with meshoptimizer: vertex cache, overdraw, then vertex fetch.
4. **LOD chain:** `meshopt_simplify` produces up to N LODs with screen-space error thresholds stored
   per LOD.
5. **Meshlets:** `meshopt_buildMeshlets` (64 verts / 124 tris) with per-meshlet bounds and normal
   cones, ready for Phase 3 GPU culling and mesh shaders. Phase 6 builds a cluster DAG on top of this.
6. **Quantization:** positions stored as 16-bit fixed point relative to the submesh AABB (or float
   for large meshes), normals and tangents octahedral-encoded (2×16-bit snorm), UVs as half floats
   when the range allows.
7. **Bounds:** per mesh, submesh, LOD and meshlet AABB plus bounding sphere.

**Cooked mesh format (`.smesh`):** a little-endian, versioned header followed by 16-byte-aligned
sections (vertex streams, indices, submeshes, LODs, meshlets, bounds). Sections are optionally
zstd-compressed, with meshopt vertex/index codecs applied first. The runtime decompresses straight
into staging memory and uploads with no per-vertex CPU work.

## 4. Texture processing (cook)

| Usage (inferred from material slot, overridable) | Color space | Format (desktop) |
|---|---|---|
| Base color (+ alpha) | sRGB | BC7 (`_SRGB`) |
| Emissive | sRGB | BC7 (`_SRGB`), or BC6H if HDR emissive |
| Normal | Linear | **BC5** (RG; Z reconstructed in shader) |
| ORM (occlusion R, roughness G, metallic B) | Linear | BC7 (or BC1 if no fine detail) |
| Single channel (height, mask, AO only) | Linear | BC4 |
| HDR (sky, IBL, lightmaps) | Linear | BC6H (UFLOAT) |

- **Mip generation** happens in linear space, using a Kaiser or box filter selected per usage.
  Normal maps are renormalized per mip. Alpha-tested textures get alpha-coverage-preserving mips.
- **Channel packing:** glTF's `metallicRoughnessTexture` (G = roughness, B = metallic) and
  `occlusionTexture` are merged into one ORM texture when their UVs and sampler match.
- **Container:** **KTX2** for cooked textures in v1 (a standard format, readable by external tools,
  supports BCn and full mip chains). Phase 4 streaming may add a tiled or paged layout.
- **Encoders:** bc7enc_rdo or the ISPC Texture Compressor for BC1–7, run in parallel over mips and
  tiles on the job system.
- **Image I/O:** stb_image (PNG/JPEG/HDR) for v1. OpenEXR is a later addition for HDR authoring.

## 5. PBR material model

**Runtime structure:**
- `MaterialTemplate`: shader (and permutation options) plus a parameter layout. It defines *what can vary*.
- `MaterialInstance`: parameter values and texture `AssetId`s for one template. At load time these
  resolve to bindless indices and are packed into a `MaterialData` entry in the GPU material buffer.

**Standard PBR template (v1):** a metallic-roughness BRDF (GGX/Trowbridge-Reitz NDF, height-correlated
Smith visibility, Schlick Fresnel, Lambert or energy-conserving diffuse, plus multiscatter energy
compensation).

| Parameter | Type | glTF source |
|-----------|------|-------------|
| baseColorFactor / baseColorTexture | float4 / tex | `pbrMetallicRoughness` |
| metallicFactor, roughnessFactor / ORM texture | float, float / tex | `pbrMetallicRoughness`, `occlusionTexture` |
| normalTexture, normalScale | tex, float | `normalTexture` |
| occlusionStrength | float | `occlusionTexture.strength` |
| emissiveFactor × emissiveStrength / emissiveTexture | float3 / tex | `emissive*`, `KHR_materials_emissive_strength` |
| alphaMode (opaque / mask / blend), alphaCutoff | enum, float | `alphaMode`, `alphaCutoff` |
| doubleSided | bool | `doubleSided` |
| clearcoat, clearcoatRoughness, clearcoatNormal | float, float, tex | `KHR_materials_clearcoat` |
| transmission, ior, thickness, attenuation | floats | `KHR_materials_transmission/_ior/_volume` |
| specular, specularColor | float, float3 | `KHR_materials_specular` |
| sheenColor, sheenRoughness | float3, float | `KHR_materials_sheen` |

`alphaMode` and `doubleSided` select pipeline state and permutations. Everything else is data in
`MaterialData`, which keeps the permutation count small for bindless rendering.

**Future templates:** layered materials and a node graph (editor), car paint (clearcoat + flakes),
terrain (height-blended layers), skin (subsurface), hair, foliage (two-sided transmission), decals,
and wetness/puddles driven by weather. All of these extend the template system without changing the
runtime contract above.

**Material source format:** a human-readable JSON `.material` file (template name, params, texture
`AssetId`s) that diffs cleanly in Git. It cooks to binary `.smat`.

## 6. Runtime loading

1. Core's streaming I/O reads the cooked bytes asynchronously (D-13/D-14).
2. The `IAssetLoader` for that type decompresses into a staging buffer, records copies on the
   **transfer queue**, and transfers ownership to graphics.
3. When the timeline semaphore signals, the resource is registered in the bindless heap and its
   handle becomes "ready". Until then, renderers use fallback resources (a gray texture, or a
   placeholder box mesh).
4. Unload is deferred until the GPU is no longer using the resource (see [rhi-design.md §4](rhi-design.md#4-memory)).

## 7. Tests

- Importer unit tests use **small glTF files generated in the test code** (single triangle, quad
  with UVs, missing normals or tangents, sparse accessors, multi-primitive), so CI needs no network access.
- Optional conformance run against the Khronos glTF-Sample-Assets set. Only assets with permissive
  licenses are used, and they're fetched in a separate, non-blocking CI job.
- Cook determinism test: cook twice and compare the bytes.
- Texture tests: sRGB vs linear handling, mip count and dimensions, BC5 normal reconstruction error
  within tolerance.
- Round trip: import → cook → load on the null backend → verify vertex counts, bounds and material parameters.
