# SAIGE Master Asset Acquisition System (001)

Zero-dependency Python 3 standard-library acquisition, validation, metadata extraction, and packaging pipeline for **SAIGE** (`https://github.com/devincombs2007-rgb/SAIGE`).

## Quick Start (Networked Runner / Claude CI)
```bash
# 1. Validate catalog & licenses without downloading
python3 saige_acquire.py --catalog catalog/acquisition_catalog.json --dry-run

# 2. Run offline unit & mock-HTTP tests
python3 -m unittest discover -s tests -v

# 3. Execute live acquisition & package SAIGE_Master_Asset_Library_001.zip
python3 saige_acquire.py --catalog catalog/acquisition_catalog.json \
  --output-dir ./saige_library_001 \
  --output-zip ./SAIGE_Master_Asset_Library_001.zip
```

## Features
- **Verified Official APIs (No API Keys / No Paid Services):**
  - [Poly Haven Public API](https://polyhaven.com/our-api) (`https://api.polyhaven.com/assets`, `/files/{slug}`, `CC0-1.0`).
  - [ambientCG v2 JSON API](https://docs.ambientcg.com/api/v2/full_json/) (`https://ambientcg.com/api/v2/full_json`, `CC0-1.0`).
- **Resilient Downloader:** HTTP `Range` resume, exponential backoff retries, streaming SHA-256 verification.
- **Metadata Extraction & Classification:** Binary `.glb` / `.gltf` / `.obj` (triangle counts, bones, animations, bounding boxes, Y-up -> Z-up notes), `.wav` (sample rate, channels, bit depth, duration), `.png` / `.exr` / `.hdr` + PBR map classification (`base_color`, `normal`, `roughness`, `metallic`, `ambient_occlusion`, `height`, `opacity`, `emissive`).
- **License & Duplicate Guards:** Rejects unverified or non-redistributable licenses and flags duplicate SHA-256 payloads.
- **Limitation Notice:** Curated web packs without public JSON download APIs (Kenney, Quaternius) are marked `manual_pack` in `catalog/acquisition_catalog.json` with zero fabricated direct URLs.
