# Acquisition safety (2026-11-27)

Why: Gemini's *Master Asset Acquisition 001* (kept unchanged in `upstream/gemini/asset_acquisition_001/`) was reviewed on #43 (comment 7209985020). Its findings
apply in part to SAIGE's own tool, `tools/production/saige_assets` (the one the 156 catalog records came from), so the corrections were made there instead of
adopting a second downloader. Gemini's catalog holds 16 *intents* (for example "Asphalt PBR set", "Kenney vehicles pack", seven of them manual packs with no
download), not resolved assets, so it adds nothing the catalog does not already cover; its ideas of resumable downloads and per-map classification are in.

## The rules (`saige_assets/safety.py`, `common.download`, `common.http_get`)
| Finding (7209985020) | In `saige_assets` before | Now |
| --- | --- | --- |
| Paths not contained | Poly Haven wrote `out_dir / <key from the API>`; asset ids became directory names | `contained()` refuses absolute paths, drive letters, backslashes, `..`, NUL and symlink escapes; ids are checked before any request |
| Unsafe archive unpacking | ambientCG used `ZipFile.extractall` | `safe_extract()`: member by member, contained, regular files only (no symlinks), at most 5000 members and 8 GiB, no member compressing better than 250:1, nothing written if any member is refused |
| No checksum, cached files trusted | any existing file was reused unchecked; no size or hash was compared | every file is compared with what the source publishes (Poly Haven: size and MD5 of every file and every glTF dependency; ambientCG: size); a cached file is reused only if it still matches, otherwise downloaded again; a mismatch deletes the download and raises |
| No URL or redirect policy | any redirect was followed | per-source HTTPS host allowlists checked on the request and every redirect (Poly Haven: `api.polyhaven.com`, `dl.polyhaven.org`; ambientCG: `ambientcg.com` and its download CDN `acg-download.struffelproductions.com`, added after checking that ambientCG's own endpoint redirects there and its site serves media from the same tree) |
| Response length not checked | no limit | 2 GiB per file (64 MiB per API response), the declared Content-Length enforced, an expected size above the limit refused before downloading |
| Resume | none | a `.part` is resumed with a Range request only if the size is known and the server answers 206 with a Content-Range that starts where the file ends; otherwise it restarts; the finished file is verified either way |
| glTF dependencies ignored | (Poly Haven already took the `include` map) | `missing_gltf_dependencies()`: every buffer and image a downloaded glTF names must exist inside its folder, or the fetch fails |
| Retry limits | 3 attempts | 3 attempts; integrity and policy failures are not retried |

Tests: `tools/production/tests/test_acquisition_safety.py` (18, offline, a local HTTP server stands in for the sources): verified download and cache, corrupt
cache re-downloaded, MD5 and size mismatches refused with nothing left behind, resume with a real Content-Range, restart when Range is ignored, size limits, a
redirect to another host refused, HTTPS required, containment cases, zip-slip, absolute members, symlink members, a zip bomb and a member-count limit refused
with nothing written, glTF dependency completeness, an 11-byte text file named `.glb` rejected, Poly Haven `include` resolution with MD5s, and hostile ids
refused before any request. Run with the other 10 production-tool tests by CTest `production_tools_python`.

## Live check (this environment, 2026-11-27)
- Poly Haven API terms read (https://github.com/Poly-Haven/Public-API/blob/master/ToS.md): free, commercial use allowed, requests must carry a unique
  User-Agent or Referer naming the software (the tool sends `saige-assets/0.1 (+https://github.com/devincombs2007-rgb/SAIGE; asset pipeline)`); the assets are CC0.
- `polyhaven fetch street_lamp_01 --res 1k`: 5 files (glTF, buffer, 3 textures), every size and MD5 matched, dependencies complete; a second fetch was served from
  the verified cache. The engine importer (`saige_asset_inspect`) loads it: 30610 triangles, four 1024x1024 textures, 3.87 m tall, `runtime_ready: true`.
- `ambientcg fetch Asphalt012 --res 1K-JPG`: the first run was **refused** because the download redirected to a host not on the list; after the CDN was verified
  and added, the 8.5 MB archive downloaded with its size checked and unpacked safely into its maps (colour, both normal conventions, roughness, displacement).
- Nothing downloaded was committed (the cache is outside the repository).

## Not done
- Khronos, MakeHuman and OpenGameArt sources go through the same bounded download but have no published hashes and no host lists yet.
- Licences are recorded per source policy (Poly Haven and ambientCG publish everything as CC0); there is no per-file licence evidence beyond that policy.
- Kenney and Quaternius packs have no download API; they stay manual entries.
