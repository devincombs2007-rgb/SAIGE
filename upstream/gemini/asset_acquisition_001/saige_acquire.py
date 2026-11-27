#!/usr/bin/env python3
"""SAIGE Automated Asset Acquisition & Preparation CLI."""

import argparse
import json
import os
from typing import List, Optional, Set

from saige_acq.downloader import AmbientCGClient, PolyHavenClient, ResilientDownloader, sha256_file
from saige_acq.extractors import (
    extract_audio_metadata,
    extract_model_metadata,
    extract_texture_metadata,
)
from saige_acq.models import AcquisitionSummary, CatalogEntry, PreparedAsset
from saige_acq.organizer import build_relative_path, package_zip, write_library_metadata
from saige_acq.validator import detect_duplicates, validate_catalog_entry


def load_catalog(path: str) -> List[CatalogEntry]:
    with open(path, "r", encoding="utf-8") as f:
        raw = json.load(f)
    return [CatalogEntry(**item) for item in raw.get("entries", [])]


def inspect_file_into_asset(entry: CatalogEntry, full_path: str, rel_path: str) -> PreparedAsset:
    ext = os.path.splitext(full_path)[1].lower().lstrip(".")
    size_bytes = os.path.getsize(full_path)
    digest = sha256_file(full_path)
    model_meta = extract_model_metadata(full_path)
    audio_meta = extract_audio_metadata(full_path)
    tex_meta = extract_texture_metadata(full_path)
    return PreparedAsset(
        asset_id=entry.asset_id,
        name=entry.name,
        category=entry.category,
        subcategory=entry.subcategory,
        relative_path=rel_path,
        source_url=entry.source_url,
        creator=entry.creator,
        license_spdx=entry.license_spdx,
        redistribution_allowed=entry.redistribution_allowed,
        file_format=ext.upper() or "BIN",
        size_bytes=size_bytes,
        sha256=digest,
        model_meta=model_meta,
        audio_meta=audio_meta,
        texture_meta=tex_meta,
    )


def run_acquisition(
    catalog_path: str,
    output_dir: str,
    output_zip: Optional[str] = None,
    dry_run: bool = False,
    categories: Optional[Set[str]] = None,
    polyhaven_base_url: str = PolyHavenClient.BASE_URL,
    ambientcg_base_url: str = AmbientCGClient.BASE_URL,
) -> AcquisitionSummary:
    entries = load_catalog(catalog_path)
    dl = ResilientDownloader()
    ph = PolyHavenClient(dl, base_url=polyhaven_base_url)
    acg = AmbientCGClient(dl, base_url=ambientcg_base_url)

    summary = AcquisitionSummary(total_catalog_entries=len(entries))
    prepared: List[PreparedAsset] = []

    for entry in entries:
        if categories and entry.category not in categories:
            continue
        ok, errs = validate_catalog_entry(entry)
        if not ok:
            summary.failed_count += 1
            summary.errors.extend(errs)
            continue

        if entry.acquisition_mode == "manual_pack":
            summary.skipped_manual_count += 1
            continue

        if dry_run:
            continue

        try:
            url = entry.download_url
            if entry.acquisition_mode == "polyhaven_api" and entry.api_slug:
                kind = "hdri" if entry.category == "hdris_vfx" else (
                    "model" if entry.category not in ("materials_pbr",) else "texture"
                )
                url = ph.resolve_download_url(
                    entry.api_slug,
                    kind=kind,
                    resolution=entry.preferred_resolution,
                    fmt=entry.preferred_format or "gltf",
                )
            elif entry.acquisition_mode == "ambientcg_api" and entry.api_slug:
                found = acg.query_materials(query=entry.api_slug, limit=1)
                if found:
                    dl_block = found[0].get("downloadFolders", {}).get("default", {}).get("downloadFiletypeCategories", {})
                    zip_block = dl_block.get("zip", {}).get("downloads", [])
                    for d in zip_block:
                        if entry.preferred_resolution.upper() in d.get("attribute", "").upper():
                            url = d.get("fullDownloadPath")
                            break
                    if not url and zip_block:
                        url = zip_block[0].get("fullDownloadPath")

            if not url:
                summary.failed_count += 1
                summary.errors.append(f"Could not resolve download URL for {entry.asset_id}")
                continue

            fname = os.path.basename(url.split("?")[0]) or f"{entry.asset_id}.bin"
            rel_path = build_relative_path(entry.category, entry.subcategory, fname)
            dest_path = os.path.join(output_dir, rel_path)
            dl.download_file(url, dest_path, expected_sha256=entry.expected_sha256)

            asset = inspect_file_into_asset(entry, dest_path, rel_path)
            prepared.append(asset)
            summary.downloaded_count += 1
            summary.total_bytes += asset.size_bytes
        except Exception as exc:  # noqa: BLE001
            summary.failed_count += 1
            summary.errors.append(f"{entry.asset_id}: {exc}")

    if not dry_run:
        summary.duplicate_count = detect_duplicates(prepared)
        write_library_metadata(output_dir, prepared, summary)
        if output_zip:
            package_zip(output_dir, output_zip)

    return summary


def main() -> int:
    ap = argparse.ArgumentParser(description="SAIGE Master Asset Acquisition CLI")
    ap.add_argument("--catalog", default="catalog/acquisition_catalog.json")
    ap.add_argument("--output-dir", default="out_library")
    ap.add_argument("--output-zip", default="SAIGE_Master_Asset_Library_001.zip")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--categories", nargs="*", default=None)
    args = ap.parse_args()

    cats = set(args.categories) if args.categories else None
    s = run_acquisition(
        catalog_path=args.catalog,
        output_dir=args.output_dir,
        output_zip=None if args.dry_run else args.output_zip,
        dry_run=args.dry_run,
        categories=cats,
    )
    print(
        f"SAIGE Acquisition Summary: total={s.total_catalog_entries} "
        f"downloaded={s.downloaded_count} manual={s.skipped_manual_count} "
        f"failed={s.failed_count} duplicates={s.duplicate_count} bytes={s.total_bytes}"
    )
    return 0 if s.failed_count == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
