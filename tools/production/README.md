# SAIGE production tools

The asset pipeline and its toolchain. Start with `docs/assets/architecture.md` (the library design) and `docs/assets/integration-report.md` (what works here and what needs the owner).

```
tools/production/setup.sh             # install everything (apt, npm, a Python venv), idempotent
source ~/.saige-tools/env.sh          # tools on PATH
python3 tools/production/doctor.py    # tools, modules, secrets present (names only), network reachability
tools/production/saige-assets --help  # the catalog and acquisition CLI
```

| Piece | Path |
| --- | --- |
| Catalog CLI and package | `saige-assets`, `saige_assets/` (sources in `saige_assets/sources/`) |
| Records, schema, taxonomy | `assets/catalog/`, `assets/schema/asset.schema.json`, `assets/taxonomy.json` |
| glTF, texture, audio, collision, vehicle, skeleton preparation | `prep/` |
| The engine importer as a runtime check | `inspect/` (`saige_asset_inspect`, built with the engine) |
| Audio mixer proof of concept | `audio_poc/` |
| Tests (offline) | `tests/` (CTest `production_tools_python`) |

Secrets are environment variables (`env.example` lists the names); none is ever stored in the repository. Generating calls to paid services refuse to run without an explicit approval flag.
