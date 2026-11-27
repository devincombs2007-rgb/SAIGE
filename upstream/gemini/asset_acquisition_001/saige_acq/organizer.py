import csv
import dataclasses
import json
import os
import zipfile
from typing import List

from .models import AcquisitionSummary, PreparedAsset


def build_relative_path(category: str, subcategory: str, filename: str) -> str:
    safe_cat = category.strip().lower().replace(" ", "_")
    safe_sub = subcategory.strip().lower().replace(" ", "_")
    return os.path.join(safe_cat, safe_sub, filename).replace("\\", "/")


def write_library_metadata(
    root_dir: str,
    assets: List[PreparedAsset],
    summary: AcquisitionSummary,
) -> None:
    os.makedirs(root_dir, exist_ok=True)

    # 1. MASTER_MANIFEST.json
    manifest_obj = {
        "schema_version": "1.0",
        "coordinate_convention": "Right-handed, Z-up, meters (SAIGE)",
        "summary": dataclasses.asdict(summary),
        "assets": [dataclasses.asdict(a) for a in assets],
    }
    with open(os.path.join(root_dir, "MASTER_MANIFEST.json"), "w", encoding="utf-8") as f:
        json.dump(manifest_obj, f, indent=2)

    # 2. ASSET_CATALOG.csv
    csv_path = os.path.join(root_dir, "ASSET_CATALOG.csv")
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(
            [
                "asset_id",
                "name",
                "category",
                "subcategory",
                "relative_path",
                "source_url",
                "creator",
                "license_spdx",
                "redistribution_allowed",
                "file_format",
                "size_bytes",
                "sha256",
                "duplicate_of",
            ]
        )
        for a in assets:
            w.writerow(
                [
                    a.asset_id,
                    a.name,
                    a.category,
                    a.subcategory,
                    a.relative_path,
                    a.source_url,
                    a.creator,
                    a.license_spdx,
                    a.redistribution_allowed,
                    a.file_format,
                    a.size_bytes,
                    a.sha256,
                    a.is_duplicate_of or "",
                ]
            )

    # 3. SHA256SUMS.txt
    with open(os.path.join(root_dir, "SHA256SUMS.txt"), "w", encoding="utf-8") as f:
        for a in sorted(assets, key=lambda x: x.relative_path):
            f.write(f"{a.sha256}  {a.relative_path}\n")

    # 4. LICENSES.md
    with open(os.path.join(root_dir, "LICENSES.md"), "w", encoding="utf-8") as f:
        f.write("# SAIGE Asset Library — Verified Licenses & Provenance\n\n")
        f.write("Powered by Poly Haven (https://polyhaven.com/our-api) & ambientCG (https://ambientcg.com/).\n\n")
        f.write("| Asset ID | Creator | License | Source URL |\n| :--- | :--- | :--- | :--- |\n")
        for a in assets:
            f.write(f"| `{a.asset_id}` | {a.creator} | `{a.license_spdx}` | {a.source_url} |\n")

    # 5. IMPORT_GUIDE.md
    with open(os.path.join(root_dir, "IMPORT_GUIDE.md"), "w", encoding="utf-8") as f:
        f.write(
            "# SAIGE Asset Import Guide\n\n"
            "- **Coordinate System:** SAIGE uses right-handed Z-up meters (`1 unit = 1 m`).\n"
            "- **glTF / GLB:** Standard glTF 2.0 assets are Y-up; apply `+90 deg` X-axis rotation on import.\n"
            "- **PBR Textures:** Follow `base_color`, `normal`, `roughness`, `metallic`, `ambient_occlusion`, `height` naming.\n"
            "- **Audio:** Prefer uncompressed PCM `.wav` (`44.1 kHz` or `48 kHz`).\n"
        )

    # 6. README.md
    with open(os.path.join(root_dir, "README.md"), "w", encoding="utf-8") as f:
        f.write(
            f"# SAIGE Master Asset Library\n\n"
            f"- **Prepared Assets:** {len(assets)}\n"
            f"- **Total Bytes:** {summary.total_bytes}\n"
            f"- **Duplicates Flagged:** {summary.duplicate_count}\n"
        )


def package_zip(source_dir: str, output_zip: str) -> str:
    os.makedirs(os.path.dirname(os.path.abspath(output_zip)), exist_ok=True)
    with zipfile.ZipFile(output_zip, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for root, _, files in sorted(os.walk(source_dir)):
            for fname in sorted(files):
                full = os.path.join(root, fname)
                rel = os.path.relpath(full, source_dir)
                zf.write(full, rel)
    return output_zip
