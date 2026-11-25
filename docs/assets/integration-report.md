# Production integration report (initial)

Environment: the cloud session's container (Ubuntu 24.04, 4 vCPU, 15 GB RAM, **no GPU**), with a limited outbound network allow-list. Measured on 2026-11-24. Raw output: `environment-snapshot.txt`
(`tools/production/doctor.py` regenerates it). Nothing here consumed a paid resource or credits; keyed services were not called (no keys, hosts not allowed).

## 1. Tools

**Installed by this work** (`tools/production/setup.sh` reproduces all of it from the Ubuntu archive, PyPI and npm; verified working):

| Tool | Version | Use | Verified by |
| --- | --- | --- | --- |
| Blender (headless, Python) | 4.0.2 (apt) | conversion, LODs, retargeting; has glTF, FBX and OBJ importers/exporters | `blender -b --python-expr` printed its Python and operators |
| glTF Transform CLI + libraries | 4.5.1 | dedup, prune, weld, simplify, texture resize, compression | `prep/gltf_optimize.mjs` on Poly Haven and Khronos models |
| meshoptimizer + gltfpack | 1.3.0 / 1.3 | LOD simplification (12,416 → 6,208 → 1,862 triangles on a rock), quantisation, meshopt compression | same run; CarConcept 11.8 MB → 4.5 MB with gltfpack |
| KTX2 / Basis Universal encoder | ktx2-encoder 0.6.0 (WASM) | GPU-texture containers, no native `toktx` and no GitHub download needed | a 1k JPEG to KTX2 UASTC with mips |
| Khronos glTF Validator | 2.0.0-dev.3.10 | spec validation | CesiumMan, CarConcept, optimised rock: 0 errors |
| FBX2glTF | 0.9.7 (npm, bundled Linux binary) | FBX (Mixamo) to glTF | `FBX2glTF --version` |
| SoX | 14.4.2 + all format libs | audio effects and analysis | installed |
| Opus tools, Vorbis tools, FLAC, LAME | libopus 1.4, vorbis 1.4.2, flac 1.4.3, lame 3.100 | audio encoders | `prep/audio_prep.py` encoded Opus and Vorbis |
| ImageMagick, libvips, OpenEXR tools | 6.9.12, 8.15.1 | image conversion, large-image resize, EXR | installed |
| optipng, pngquant, webp | 0.7.8, 2.18.0, 1.3.2 | texture compression | installed |
| assimp | 5.3.1 | format conversion | installed |
| Python libraries | numpy, scipy, pillow, opencv, trimesh, pygltflib, xatlas, fast-simplification, networkx, librosa, soundfile, pyloudnorm, mutagen, httpx | geometry, UVs, audio analysis, loudness | the unit tests and prep tools use them |
| miniaudio | 0.11.26 (header, public domain / MIT-0) | audio mixer candidate | proof of concept builds and passes numerical checks |

**Already available:** FFmpeg 6.1.1, ImageMagick 6.9 (`convert`), git-lfs 3.4.1, glslang 15.1, CMake 3.28, Ninja, Node 22, Python 3.13.

**Not available / not usable here:**

| Tool | Why | Path forward |
| --- | --- | --- |
| MakeHuman application | needs a GUI (Qt/OpenGL); the PyPI wheel `makehuman` 1.3.2 ships code and morph targets but not the base meshes and assets | use the CC0/CC-BY community assets (indexed here) with a Blender add-on |
| MPFB (MakeHuman for Blender) | current builds are Blender extensions for **Blender 4.2+**; apt provides 4.0.2. I stopped before running an older legacy build of third-party add-on code that the session's safety review did not allow me to execute without your say-so | a Blender 4.2+ (blocked: `download.blender.org`) or your explicit approval to test a legacy MPFB release; Codex or you can decide |
| `bpy` as a Python module | PyPI wheels are for Python 3.11; the container has 3.13/3.12 | not needed: headless `blender -b` works |
| Native `toktx` / KTX-Software | GitHub release downloads are blocked | the WASM encoder replaces it |
| Texture/animation retargeting tools | Blender's own constraints/bake do the job (script in progress: `skeleton_validate.py` maps a rig to a canonical humanoid; no retarget has been run because no Mixamo data is available) | needs the first Mixamo FBX |
| TRELLIS / Hunyuan3D / other local 3D generators | no GPU | see §7 |

Additional free tools worth having (not installed): Material Maker and Materialize (PBR authoring, GUI), CMU Graphics Lab motion capture (free for any use; animation source if Mixamo is awkward), RNNoise through FFmpeg's `arnndn` (denoising, already in FFmpeg), Essentia (audio feature extraction, heavy), Draco (apt `draco`, only if we choose KHR_draco), OpenImageIO (`oiio-tools`), Steam Audio (Apache-2.0, HRTF and propagation, later), Cascadeur (character physics animation, free tier, GUI).

## 2. Services

Reachability was tested with a plain HTTPS request through the session proxy ("BLOCKED" = the network policy denies the host).

| Service | Reachable now | Needs a key | Scripted by us | Notes |
| --- | --- | --- | --- | --- |
| Poly Haven (`polyhaven.com`, `api.polyhaven.com`, `dl.polyhaven.org`) | yes | no | **yes**, tested: search and fetch of models, PBR textures, HDRIs | CC0; 522 models, hundreds of materials and HDRIs |
| ambientCG (`ambientcg.com`, `docs.ambientcg.com`) | yes | no | **yes**, tested: search and fetch of a material | CC0; ~2,000 assets |
| Khronos glTF Sample Assets (`raw.githubusercontent.com`) | yes | no | **yes**, tested: 153-model index, fetch of CesiumMan and CarConcept with licence parsing | per-model licences |
| MakeHuman community assets (`files.makehumancommunity.org`) | yes (allowed this session) | no | **yes**, tested: search of the 3,406-asset index (549 CC0 + 804 CC-BY clothes, 775 CC0 materials, 241 CC0 poses, rigs, targets, 45 CC0 models) | MakeHuman formats; needs Blender ≥ 4.2 and MPFB to become glTF characters |
| OpenGameArt (`opengameart.org`) | yes (allowed) | no | **yes**, tested on a CC0 content page (files, licence, author); no official API and no reliable search, so you or an agent name pages | licences vary per item |
| Kenney (`kenney.nl`) | yes (allowed) | no | **no**: the free download link comes from a signed endpoint that returns 403 to scripts; I did not try to get around that | browser download; many packs are mirrored on OpenGameArt, which we can script |
| Quaternius (`quaternius.com`) | yes (allowed) | no | no: the static page exposes only Patreon/Twitter links | browser download |
| Sonniss GameAudioGDC (`sonniss.com`) | host allowed, but **403 from the site's own bot protection** | no | no | manual download in a browser; the bundles are tens of GB |
| Freesound (`freesound.org`) | BLOCKED | **API key** (free); OAuth2 for original-quality files | client written, **unverified** | CC0/CC-BY filters; about 60 requests/min and 2,000/day reported |
| ElevenLabs (`api.elevenlabs.io`) | BLOCKED | **API key** | client written, unverified, refuses to spend | free plan: non-commercial, attribution |
| Tripo AI (`api.tripo3d.ai`) | BLOCKED | **API key** (separate API credits) | balance check written, unverified | see `ai-generation.md` |
| Meshy (`api.meshy.ai`) | BLOCKED | key, **paid plan only** | balance check written | skipped |
| Sketchfab (`api.sketchfab.com`) | BLOCKED | **API token** (your account) | search/token check written, unverified | per-model licences |
| Higgsfield | not tested (hosts unknown to the allow-list) | OAuth (MCP) or key+secret | no | costs credits |
| Mixamo (`www.mixamo.com`) | BLOCKED | Adobe login | **no, and not sanctioned** | manual download |
| Pixabay (`pixabay.com`) | BLOCKED | none for downloads (manual), API has no audio | no | manual |
| CGTrader (`www.cgtrader.com`) | BLOCKED | login | no | manual, per-model licence |
| Blender releases (`download.blender.org`), Hugging Face (`huggingface.co`), GitHub release assets (`github.com/<other repos>`) | BLOCKED | no / token | n/a | needed only for Blender ≥ 4.2, open 3D models (GPU anyway) and some third-party tools |

**MCP and CLI integrations:** Higgsfield offers an official MCP server (`https://mcp.higgsfield.ai/mcp`, OAuth). The other services have REST APIs and community MCP servers of unknown provenance; I did not install
any third-party MCP server (each runs code with your keys). Poly Haven, ambientCG, Khronos and MakeHuman need no MCP: `saige-assets` calls them directly. A Freesound Python client exists (`mtg/freesound-python`), but a thin client in `keyed.py` is enough and keeps the dependency list short.

## 3. Free quotas and commercial terms (as published; verify before relying)

| Service | Free allowance | Commercial use | Redistribution |
| --- | --- | --- | --- |
| Poly Haven / ambientCG | unlimited (CC0) | yes | yes |
| Freesound | API key free; reported 60 req/min, 2,000/day, 500 original downloads/day; terms say conditions can change and ask not to register multiple keys | per sound licence; CC0 and CC-BY yes | CC0/CC-BY yes, never NC |
| ElevenLabs | monthly free credits on the free plan | **no** on the free plan (paid plans yes) | attribution required on free |
| Tripo | web free plan with monthly credits; API credits unclear | free-plan output is CC BY 4.0 without commercial rights | per plan |
| Meshy | web only; **API needs a paid plan** | per plan | per plan |
| Higgsfield | credits deducted for MCP/API | per its Terms | per its Terms |
| Mixamo | free with an Adobe account | yes, in your product | **no**, and no AI training |
| Sonniss | free bundles | yes | embedded in a product only; recent bundles forbid AI training |
| Pixabay | free | yes | no standalone redistribution |
| Sketchfab | account-bound downloads | per model licence | per model licence |

## 4. Network permissions

Added this session (with your approval): `kenney.nl`, `quaternius.com`, `opengameart.org`, `makehumancommunity.org`, `www.makehumancommunity.org`, `static.makehumancommunity.org`, `files.makehumancommunity.org`,
`files2.makehumancommunity.org`, `extensions.blender.org`, `sonniss.com`. Needed next, one per service you give a key for (saving a key through the secure card allows its host by itself):
`freesound.org` (and `cdn.freesound.org` for previews), `api.elevenlabs.io`, `api.tripo3d.ai`, `api.sketchfab.com` (and `media.sketchfab.com` for downloads). Optional: `download.blender.org` (Blender 4.2+),
`github.com` release assets, `huggingface.co` (open 3D models; only useful with a GPU).

## 5. Hardware limits

No GPU: rendering is software Vulkan (llvmpipe), 4 cores. Local neural 3D/texture generation (TRELLIS, Hunyuan3D, Stable Diffusion-class models) is not possible here; it needs a machine with a 16-24 GB GPU
that the owner would have to provide or pay for. Everything in the pipeline above is CPU-only.

## 6. Recommended storage architecture

See `architecture.md`: public catalog (metadata, hashes, licences) + scripted re-acquisition into a verified cache for everything redistributable + a **private** repository for what cannot be fetched or published
(Mixamo, Sonniss, Pixabay, manual downloads) + a bucket later only if volume demands it. No binaries in the public repository.

## 7. Delivered in this change

* `tools/production/setup.sh`, `doctor.py`, `package.json`, `requirements.txt` (reproducible toolchain, shareable with Codex).
* `saige-assets` CLI and `saige_assets` package: catalog (schema, taxonomy, persistent ids, version history), sources for Poly Haven, ambientCG, Khronos, MakeHuman, OpenGameArt, clients for the keyed services,
  technical metadata extractors (models, images, HDRIs, audio), `validate` (Khronos validator + the engine's importer), duplicate detection, hash verification, licence report.
* `saige_asset_inspect` (C++, CTest-built): the engine's glTF importer as a runtime check. It rejected a meshopt-compressed car correctly and accepted the same car without compression.
* Prep tools: `gltf_optimize.mjs` (dedup/prune/weld/texture limit/LODs/profiles), `ktx2_encode.mjs`, `texture_prep.py` (ORM packing, normal convention), `audio_prep.py` (loudness, loops, encode), `collision_hull.py`, `vehicle_analyze.py`
  (finds wheels, doors, hood, steering; measures wheelbase, track, radius; suggests `VehicleConfig`: it reproduced the CarConcept numbers I had measured by hand), `skeleton_validate.py`.
* A seed catalog of 26 records (the showcase's textures and props, CarConcept, CesiumMan, an HDRI), 13 models validated against the engine importer.
* `tools/production/audio_poc`: miniaudio feasibility, verified numerically.
* 10 offline unit tests (CTest `production_tools_python`).

## 8. Recommended priorities

0. **Animations are solved without an account:** Quaternius's Universal Animation Library 1 and 2 (CC0, on OpenGameArt) load in the engine importer (one 53- and one 65-joint skin, 46 and 43 clips: idle, jog, sprint, jump start/loop/land, driving, sitting enter/idle/exit, crouch, push, pick-up, hit, death, slides, combat), plus KayKit's character animations. Mixamo is optional now.
1. You: the private repository, then the four API keys through the secure cards (Freesound first: it unlocks recorded sound effects), Mixamo animations downloaded once, and the decisions in `owner-checklist.md`.
2. Freesound CC0 recorded vehicle, impact, foley and ambience sets, catalogued and prepared; re-render Showcase 03's soundtrack from real recordings through the offline miniaudio mix.
3. Poly Haven/ambientCG sweep by taxonomy area (vehicles are scarce there: Sketchfab CC0/CC-BY with your token), validated and registered.
4. Characters: Blender ≥ 4.2 with MPFB (needs your decision on the add-on or the Blender download), Mixamo retargeting.
5. Codex: runtime audio on miniaudio (design in `audio.md`), asset streaming and budgets driven by the catalog's technical data, KTX2/meshopt support in the importer so the `distribution` profile becomes loadable.

## Sources

* [Tripo OpenAPI platform announcement](https://www.tripo3d.ai/blog/announcement-tripo-openapi-platform-payment-system) and [pricing comparison](https://www.3daistudio.com/3d-generator-ai-comparison-alternatives-guide/ai-3d-generator-pricing-compared)
* [Meshy API pricing](https://docs.meshy.ai/en/api/pricing) and [Meshy free plan](https://intercom.help/meshy/en/articles/15696428-what-is-included-on-the-free-plan)
* [ElevenLabs: can I publish generated content](https://elevenlabs.io/docs/help-center/legal/can-i-publish-the-content-i-generate-on-the-platform)
* [Freesound API overview](https://www.freesound.org/docs/api/overview.html) and [terms of use](https://freesound.org/docs/api/terms_of_use.html)
* [Adobe Mixamo additional terms](https://wwwimages2.adobe.com/content/dam/cc/en/legal/servicetou/Mixamo-Addl-Terms-en_US-20210623.pdf) and [Mixamo FAQ](https://helpx.adobe.com/en/creative-cloud/faq/mixamo-faq.html)
* [Higgsfield API blog](https://higgsfield.ai/blog/higgsfield-api)
* [Sonniss GameAudioGDC licence summary](https://licenseorg.com/guide/music-audio/sonniss)
* [Sketchfab Download API](https://api.sketchfab.com/developers/download-api)
* [Hunyuan3D 2](https://github.com/Tencent/Hunyuan3D-2) and [TRELLIS requirements](https://docs.clore.ai/guides/3d-generation/trellis-3d)
* [Pixabay content licence](https://pixabay.com/en/service/license/)
