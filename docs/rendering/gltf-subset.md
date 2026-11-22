# glTF 2.0 import: the supported subset (v1)

**Status:** implemented in `engine/gfx_assets` (CPU data only: no GPU upload, no image decoding yet).
Agreed with Core in [#17](https://github.com/devincombs2007-rgb/SAIGE/issues/17), contract C-2.

The importer accepts a *defined subset* of glTF 2.0 and says so precisely when it meets something outside it. It never
guesses, never crashes on malformed input, and never reads outside the asset's own directory.

## Containers and resources
| Feature | v1 |
|---|---|
| `.gltf` (JSON) with external `.bin` / images | ✅ |
| `.glb` (binary container, version 2, JSON chunk plus optional BIN chunk) | ✅ |
| `data:` URIs (base64) for buffers and images | ✅ (strict base64; no whitespace) |
| External resource URIs | ✅ **only** plain relative paths inside the asset's directory (see below) |
| `asset.version` | `2.x` (anything else is an error) |

### Resource URI policy (security)
A resource URI is percent-decoded once, then **rejected** if it is empty, longer than 1024 bytes, absolute (leading `/`),
contains a drive letter or scheme (`:`), a backslash, a NUL or other control character, an empty segment (`a//b`), or a `..`
segment. `.` segments are dropped. `data:` URIs are handled separately. A rejected URI is a load **error**, not a
warning. The file-based entry point additionally resolves every path against the asset's directory and refuses anything that
resolves outside it, so a symlink cannot escape either.

## Scene and nodes
| Feature | v1 |
|---|---|
| Default scene (`scene`, else scene 0; with no scenes, every root node) | ✅ |
| Node hierarchy: `children`, `matrix` **or** `translation`/`rotation`/`scale` | ✅ |
| Mesh references on nodes, flattened to world-space instances | ✅ |
| Node graph must be a forest (no cycles, no shared children), depth ≤ 256 | ✅ enforced |
| Cameras, lights, skins, animations, morph targets (`weights`) | ⚠️ ignored with one warning each. Skeletal data is out of scope until the animation contract (#7) |

## Meshes
| Feature | v1 |
|---|---|
| Primitive `mode` | `4` (TRIANGLES) only. Other modes are skipped with a warning |
| `POSITION` | `VEC3` `FLOAT` (required) |
| `NORMAL` | `VEC3` `FLOAT`. If absent, **flat normals are generated** as the glTF spec requires (the primitive is de-indexed) |
| `TANGENT` | `VEC4` `FLOAT`. If a primitive's material has a normal texture and the file has none, tangents are **generated** at import (Lengyel-style, deterministic; not MikkTSpace, see `tangents.hpp`) with a warning. A normal texture with no `TEXCOORD_0` is warned about and generates nothing. Primitives whose material has no normal texture get none |
| `TEXCOORD_0` | `VEC2`: `FLOAT`, or normalized `UNSIGNED_BYTE` / `UNSIGNED_SHORT` |
| `COLOR_0` | `VEC3`/`VEC4`: `FLOAT`, or normalized `UNSIGNED_BYTE` / `UNSIGNED_SHORT` |
| Other attributes (`TEXCOORD_n`, `JOINTS_0`, `WEIGHTS_0`, ...) | ignored with a warning |
| Indices | `UNSIGNED_BYTE` / `UNSIGNED_SHORT` / `UNSIGNED_INT`, validated against the vertex count. Absent = sequential |
| Strided `bufferView`s (`byteStride`) and arbitrary `byteOffset`s | ✅, with overflow-checked bounds |
| **Sparse accessors** | ❌ rejected with a clear error |
| Accessors without a `bufferView` (spec: all-zero data) | ❌ rejected with a clear error |
| `KHR_mesh_quantization`, `KHR_draco_mesh_compression`, `EXT_meshopt_compression` | ❌ required-extension error |

## Materials and textures (as data; images stay encoded)
| Feature | v1 |
|---|---|
| `pbrMetallicRoughness`: `baseColorFactor`/`Texture`, `metallicFactor`, `roughnessFactor`, `metallicRoughnessTexture` | ✅ with spec defaults (metallic 1, roughness 1) |
| `normalTexture` (+`scale`), `occlusionTexture` (+`strength`), `emissiveFactor`/`Texture` | ✅ |
| `alphaMode` (`OPAQUE`/`MASK`/`BLEND`), `alphaCutoff`, `doubleSided` | ✅ `BLEND` is composited with sorted source-over alpha blending ([alpha-blending.md](alpha-blending.md)); with a scene format that cannot blend it draws opaque, with a stage warning |
| `KHR_materials_emissive_strength` | ✅ (folded into the emissive factor) |
| Textures: `source`, `sampler` (filters, wrap modes); images from relative URI, `data:` URI or `bufferView` | ✅ kept as **encoded bytes plus MIME type**. PNG/JPEG decoding is a separate, bounded step: `decode_image` and `generate_mips` in `saige/gfx_assets/image.hpp` (pinned stb_image, see `docs/third-party/stb_image.md`) |
| `texCoord` other than 0 | ⚠️ kept in the data, warned: the renderer only samples `TEXCOORD_0` |
| `KHR_texture_transform`, `KHR_materials_*` (other than emissive strength) | ⚠️ optional uses are ignored with a warning |

**What the renderer shades** (`stage_model`, milestones M1 to M3 of [the material architecture](material-architecture.md)): all
five textures are decoded and bound, each with the sampler its glTF texture names. Base color and emissive are uploaded as
sRGB, metallic-roughness, normal and occlusion as linear data (the same image used in both roles is uploaded once per color
space). Shaded in both modes: base color, emissive (texture times factor) and occlusion (scales the ambient term by
`strength`). Shaded only when the renderer is created with `RendererSettings::model_shading = ModelShading::MetallicRoughness`:
- **Metallic-roughness** (G roughness, B metallic, times the factors): GGX, height-correlated Smith and Schlick as in the glTF
  specification, the sun, and a hemispherical ambient through a split-sum environment BRDF table.
- **Normal mapping**: the tangent-space normal (x and y times `normalTexture.scale`, +Y up in the image) is rotated by the
  vertex tangent frame. The frame is orthonormalized against the interpolated normal; the bitangent's handedness comes from
  the transformed frame, so mirrored instances stay correct; the back of a double-sided surface negates the whole frame, as the
  glTF reference viewer does. A primitive without tangents (no texture coordinates) skips the map, and staging reports it.
  Tangents come from the file or from the importer's generator.

The default is `ModelShading::Legacy`, the 0.2 look (albedo times ambient plus sun, no normal maps), so existing scenes are
unchanged until a host opts in. Mip chains for linear data use the same box filter as color, so normal maps are not renormalized
per level (a known softening of distant relief; Toksvig-style filtering is a later refinement). By default the target is display-referred
and clips: highlights above 1 saturate. Setting `RendererSettings::hdr` draws the scene in linear light into an RGBA16_FLOAT
target and resolves it to the output with a tone-map pass (ACES-inspired by default, with `tone_map.exposure_ev`; see
`saige/render/tonemap.hpp`). Specular ambient ignores the lobe's blur until image-based lighting (M5).

## Extensions
- Every entry of `extensionsRequired` must be supported, otherwise the load **fails** naming the extension.
  Supported required extensions in v1: `KHR_materials_emissive_strength`.
- Unsupported `extensionsUsed` that are not required are ignored with a warning.

## Coordinate conversion (done once, at import)
glTF is right-handed with **+Y up** and the asset's **front facing +Z** (so the asset's left is +X). The engine is
right-handed **Z-up, +X forward, +Y left** ([standard](../standards/coordinate-system.md)). The importer converts
everything to engine space with a single cyclic permutation:

```
(x_engine, y_engine, z_engine) = (z_gltf, x_gltf, y_gltf)
```

It is a **proper rotation** (determinant +1), equal to +90° about X followed by +90° about Z, so **triangle winding is
preserved**: counter-clockwise stays counter-clockwise. Positions, normals, tangent directions and bounds are converted at
import; node transforms are conjugated (`M_engine = C * M_gltf * C^-1`) so instances place the converted meshes correctly.
UVs need no change: glTF's texture origin is top-left, the same as Vulkan's.

## Limits (configurable; all enforced before large allocations)
Maximum file size, buffer size, vertices per primitive and node depth are checked against the declared sizes first, so a
hostile file cannot make the loader allocate gigabytes. All offsets are bounds-checked with overflow-safe arithmetic.

## Not yet supported (tracked)
MikkTSpace-exact tangents (the generator is Lengyel-style; adopting MikkTSpace means pinning a third-party dependency), image decoding, `KHR_texture_transform`, sparse accessors,
skins/animation/morph targets, cameras and lights, `.gltf` files that reference other `.gltf` files, and Draco/meshopt.
