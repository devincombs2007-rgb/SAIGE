# External CC0 asset sources: access check and importer test picks (2026-11-23)

Purpose: find real, permissively licensed content to test SAIGE's glTF importer and PBR path against, without putting binaries in the
repository. **Nothing from these sites is committed.** `tools/assets/fetch_sample_assets.py OUT_DIR` downloads exactly the two picks below
(about 5.6 MB) and checks their SHA-256 against the values here.

## Access (from the cloud development environment)

| Host | Result | Used for |
| --- | --- | --- |
| `api.polyhaven.com` | works once allowed (`GET /types`, `/assets?t=models` 522 models, `/info/{id}`, `/files/{id}`) | catalog, metadata, exact file URLs |
| `polyhaven.com` | works once allowed (home page 200) | license page |
| `ambientcg.com` | works once allowed (`/api/v3/assets`, and `/get?file=` answers 302) | catalog, download links |
| `docs.ambientcg.com` | works once allowed (v3 API docs, license page) | API and license documentation |
| `dl.polyhaven.org` | **needed in addition**: every Poly Haven file URL in the manifest points here | downloads |
| `acg-download.struffelproductions.com` | **needed in addition**: ambientCG's `/get` redirects here | downloads |

By default the environment's egress proxy refuses all of these (`CONNECT` 403). They were allowed through a personal "SAIGE assets" environment
(the six hosts above, nothing else, package registries unchanged). A session without them gets the 403 and cannot browse or download; the safe alternatives are the generated
original assets under `showcase/01-courtyard` (no network needed) or fetching the two picks on a machine with open access and attaching them outside git.
Both catalogs have documented JSON APIs and official download links; only those were used, one catalog listing each and the two picks (no bulk download).

## Licenses

- **Poly Haven**: every asset is CC0 (https://polyhaven.com/license: "Our assets are all licensed as CC0"). `marble_bust_01`: author Rico Cilliers, published 2021-07-14.
- **ambientCG**: "All ambientCG assets are provided under the Creative Commons CC0 1.0 Universal License" (https://docs.ambientcg.com/license/), downloadable files and previews included.
  Credit is not required; naming the source in the project's credits is still courteous.

## Picks

### Model: Poly Haven `marble_bust_01`, 1k glTF bundle (about 0.9 MB)

17,456 triangles (the API's `polycount`), one mesh, one PBR material, sized in meters (0.30 x 0.30 x 0.52 m by the importer's bounds). Files (all listed by `/files/marble_bust_01`;
note the textures live under `/Models/jpg/1k/...`, not beside the `.gltf`):

| File | Bytes | SHA-256 |
| --- | --- | --- |
| `marble_bust_01_1k.gltf` | 2,793 | `b6f192fc...97ff09b` |
| `marble_bust_01.bin` | 416,608 | `0bb757c1...59e33` |
| `textures/marble_bust_01_diff_1k.jpg` | 160,337 | `cbb7dccd...3383c` |
| `textures/marble_bust_01_nor_gl_1k.jpg` | 124,627 | `3d355904...37c5f` |
| `textures/marble_bust_01_rough_1k.jpg` | 193,918 | `1c450235...921fb` |

Higher resolutions (2k/4k/8k) use the same `.bin`; the 8k bundle is about 17 MB of JPEG. Other formats offered: `.blend`, `.fbx`, `.usd` (SAIGE imports none of them).

### Material: ambientCG `Rock063`, 1K-JPG (4,979,523 bytes, SHA-256 `e2974a91...19656`)

Photogrammetry rock, 1024 x 512, tileable. The zip holds loose maps (`Color`, `NormalGL`, `NormalDX`, `Roughness`, `AmbientOcclusion`, `Displacement`; JPEG, the single-channel ones are
grayscale) plus `.blend`, `.usdc`, `.mtlx` and `.tres` material descriptions and a preview PNG. There is **no glTF material**. Larger sizes: 2K 14.9 MB, 4K 50.7 MB, 8K 173 MB; PNG variants are roughly double.

## What SAIGE's importer and renderer do with them today (measured with a throwaway program on lavapipe, validation on: 0 errors)

- **`marble_bust_01`**: imports (1 mesh, 1 material, 3 textures, 29,238 vertices, 52,368 `uint16` indices) and stages in `MetallicRoughness` shading (3 textures, about 16 MiB of GPU memory at 1k, uncompressed RGBA8 with mips).
  Importer warning: the normal-mapped primitive has no tangents, so they were generated (Lengyel-style, not MikkTSpace; the MikkTSpace option is in #58).
  Things to know: the material is `doubleSided`; the "rough" JPEG is a packed ARM texture (AO in R, roughness in G, metallic in B) that the glTF wires only as `metallicRoughnessTexture`, so **ambient occlusion is not used**
  (there is no `occlusionTexture`); `metallicFactor` is 0, which is what keeps B from adding metal.
- **`Rock063`**: staged through a hand-written one-quad glTF in scratch (4 textures, 10.7 MiB): the JPEGs decode (baseline, 1024 x 512, well inside the limits). Caveats: SAIGE has **no loose-map material description**; the `.mtlx`/`.usdc`/`.tres`/`.blend` are not read,
  so every ambientCG material needs a glTF written for it. The grayscale `Roughness` JPEG decodes to R=G=B=roughness, so as a `metallicRoughnessTexture` it would also drive the metallic channel unless `metallicFactor` is 0 (the test used 0); channel packing (AO/roughness/metal)
  belongs in an offline tool. `Displacement` has no consumer (no parallax or tessellation). Use `NormalGL` (glTF's convention), not `NormalDX`.
- **Not supported, so do not assume** (from `docs/rendering/gltf-subset.md`): `KHR_texture_transform` and other `KHR_materials_*` (ignored with a warning), sparse accessors (rejected), Draco/meshopt/quantization (rejected), skins, animation, morph targets, cameras, lights (ignored),
  second UV sets (ignored), KTX2/Basis textures (no decoder, only JPEG and PNG), `.fbx`/`.blend`/`.usd` import. Sun shadows (#76) are opt-in and the bust would cast opaque shadows; there is no image-based lighting yet, so a marble scan will look flatter than on the source site.

## Next steps (not done here)

- A small `tools` step that packs AO/roughness/metallic into one linear texture and writes the glTF for a loose-map material, so ambientCG materials become importable.
- Decide whether the bust becomes a *private-by-reference* test (fetched by the script in CI with hash checks) or whether a few KB of generated stand-ins keep CI offline; large binaries stay out of the repository either way.
