# The SAIGE shared asset library

One library for Claude Code and Codex, built so that a session can find, acquire, check, import and register an asset without the owner locating files by hand, and so that the public engine
repository never carries a file whose licence forbids it. Tooling: `tools/production` (setup, `saige-assets`, prep tools, `saige_asset_inspect`). Catalog: `assets/catalog`.

## What lives where

| Thing | Where | Why |
| --- | --- | --- |
| Code, tools, schema, taxonomy | the public engine repository | it is ours |
| **Catalog records**: one JSON file per asset with id, source URL, creators, licence, file hashes, technical data, status, version history and the command that reacquires it | the public repository, `assets/catalog/<category>/<id>.json` | metadata and hashes are facts; they make a build reproducible and a review possible, and they weigh a few KB each |
| **Binaries whose licence allows redistribution and that a script can fetch again** (Poly Haven, ambientCG, Khronos samples, MakeHuman community CC0/CC-BY, CC0 OpenGameArt) | nowhere in git: reacquired from the source by `saige-assets fetch ...` into a content-checked cache (`SAIGE_ASSET_CACHE`, default `~/.cache/saige-assets`) | no repository growth, no duplication, the source stays the authority; every fetch is verified against the recorded SHA-256 |
| **Binaries that cannot be fetched by script or may not be redistributed** (Mixamo downloads, Sonniss GameAudioGDC, Pixabay audio, CGTrader, Kenney/Quaternius zips fetched in a browser, AI-service output with restricted terms, and their derivatives) | a **private** repository `saige-assets-private` (to be created by the owner, see the checklist), cloned to `SAIGE_PRIVATE_ASSETS_DIR`; its own catalog records carry `redistribution: private-only` or `embedded-only` | keeps them out of the public repository; both agents reach it through the same environment secret |
| Derived runtime assets (optimised glTF, KTX2, Opus) | built by the prep tools from the sources, never committed unless the licence is `allowed*` and the file is small | reproducible from the source plus the command in the record |

**Why not one big store.** The engine repository is public: a committed texture is redistributed to everyone forever, and Git history never forgets. Most of what we need is CC0 and one HTTP request away, so
re-downloading is cheaper than storing it. The private repository exists only for what cannot be re-downloaded or must not be public. If it ever outgrows what a private repository handles comfortably
(a few GB: Git LFS on a free account is 1 GiB of storage and 1 GiB of bandwidth a month, and more costs money, which needs the owner's approval), move the blobs to an S3-compatible bucket
(Cloudflare R2 or Backblaze B2 have free tiers; not enabled, needs an account) and keep the same catalog records: `files[].path` is cache-relative, so the store behind the cache can change without touching a record.

## Record identity and versions

* `id = <kind>.<source>.<slug>` is persistent. It never changes.
* A re-fetch that produces different bytes bumps `version` and appends the retired hashes to `history`, so a project file can pin `id@version` and a build can name the exact bytes.
* `status`: `candidate` (acquired, not yet judged) → `approved` (used or cleared for use) / `rejected` (with the reason in `suitability.notes`) / `superseded`. Fetching again never resets an approval.
* `suitability.runtime_ready` is set by `saige-assets validate`: the Khronos glTF validator **and the engine's own importer** (`saige_asset_inspect`) must accept the file. A model the importer rejects
  (an `EXT_meshopt_compression` file, a second skin, an undecodable texture) is not game-ready however good it looks; the verdict says why.

## Record fields worth knowing

`license.redistribution` says what the public repository may do: `allowed` (CC0), `allowed-with-attribution` (CC-BY: keep `license.attribution` in the credits), `embedded-only` (royalty-free inside a
product, no standalone redistribution: Sonniss, Pixabay), `private-only`, `forbidden`, `unknown` (treat as private until a human reads the licence). `technical` carries what a budget needs before import:
triangles, vertices, materials, per-texture size and GPU bytes, clips with durations, skins with joint counts, bounds; for audio duration, sample rate, channels, peak, integrated loudness (LUFS), silence
ratio and loop-seam smoothness.

## The workflow (what an agent does)

1. **Requirement.** Name the need and its budget (for example "street lamp, under 10k triangles, textures under 8 MiB, CC0").
2. **Search the library first.** `saige-assets find lamp --kind model --redistributable`.
3. **Search the approved sources.** `saige-assets search polyhaven "lamp"`, `... ambientcg`, `... khronos`, `... makehuman`, `... freesound` (key), `... sketchfab` (key). Prefer professionally authored, scanned or photogrammetry assets.
4. **Evaluate** quality (look at it), licence (read `license` in the fetched record; unknown means stop), and technical fit (`technical` in the record).
5. **Acquire.** `saige-assets fetch polyhaven street_lamp_01 --res 1k` (downloads, hashes, describes, writes the record as `candidate`).
6. **Process.** `tools/production/prep/gltf_optimize.mjs` (dedup, prune, weld, texture resize, LODs), `prep/texture_prep.py` (ORM packing, normal convention), `prep/audio_prep.py` (loudness, loop, encode), `prep/collision_hull.py`, `prep/vehicle_analyze.py`, `prep/skeleton_validate.py`.
7. **Import into SAIGE** (the engine's glTF importer; materials and props as in `showcase/03-horizon`).
8. **Validate runtime behaviour.** `saige-assets validate ID` (validator + importer), then the render or the test that uses it; `saige-assets approve ID --reason ...` once it works.
9. **Generate a custom asset only when nothing suitable exists** (Tripo, a future local model), record it like any other asset with its generator, prompt and the service's licence terms.

## Responsibilities

| | Claude Code | Codex |
| --- | --- | --- |
| Library and pipeline | acquisition, quality judgement, materials and textures, character models, animation preparation, environment production, AI-generated visual assets, the pipeline code | runtime asset management (loading, streaming, residency, budgets), memory management, instrumentation, automated validation in CI |
| Audio | acquisition, preparation, the sound design mapping | the audio runtime architecture (mixer, voices, spatialisation, streaming), event wiring |
| Shared | the schema, the catalog and the tools; changes to them are proposed on issue #43 | |

## Rules

* Never commit a binary whose record says anything but `allowed` / `allowed-with-attribution`, and prefer not committing those either (re-fetch).
* Never put a key, token or password in git, in a record or in a log. Secrets are environment secrets (checklist).
* Never spend credits (Tripo, ElevenLabs, Higgsfield, Meshy) without the owner's explicit approval; `keyed.py` refuses generating calls unless `confirm_spend=True`.
* A licence the tool cannot read is `unknown`, and unknown is private until a person reads it.
* Keep source attribution in the credits of anything we ship (`license.attribution`).
