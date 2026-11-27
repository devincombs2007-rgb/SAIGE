# Integration Guide — SAIGE Master Asset Acquisition (For Claude)

1. **Execution Environment:** Run `saige_acquire.py` on a network-enabled workstation or CI runner (Python 3.10+, stdlib only — no `pip install` needed).
2. **Coordinate System:** SAIGE uses right-handed Z-up meters (`1 unit = 1 m`). `extractors.py` flags glTF 2.0 `Y-up` assets so the SAIGE importer applies `+90 deg` X-axis rotation on import.
3. **Output Structure:** `saige_acquire.py` produces `MASTER_MANIFEST.json`, `ASSET_CATALOG.csv`, `SHA256SUMS.txt`, `LICENSES.md`, `IMPORT_GUIDE.md`, `README.md`, and category folders (`vehicles/`, `buildings/`, `interiors/`, `vegetation/`, `characters_animations/`, `audio/`, `materials_pbr/`, `hdris_vfx/`, `environment_props/`).
4. **Extending the Catalog:** Add `polyhaven_api` slugs from `PolyHavenClient.list_assets()` or `ambientcg_api` queries from `AmbientCGClient.query_materials()`, or place manually downloaded `CC0` packs (`kenney.nl`, `quaternius.com`) into the output folder and re-run `inspect_file_into_asset()` + `write_library_metadata()`.
