from typing import Dict, List, Tuple

from .models import CatalogEntry, PreparedAsset


ALLOWED_LICENSES = {
    "CC0-1.0",
    "CC-BY-4.0",
    "CC-BY-3.0",
    "PUBLIC-DOMAIN",
    "QAL-1.0",  # Quaternius Asset License (free personal & commercial use)
}


def validate_catalog_entry(entry: CatalogEntry) -> Tuple[bool, List[str]]:
    errors: List[str] = []
    if not entry.asset_id.strip():
        errors.append("Missing asset_id")
    if not entry.source_url.startswith(("https://", "http://")):
        errors.append(f"Invalid source_url for {entry.asset_id}: {entry.source_url}")
    if not entry.redistribution_allowed:
        errors.append(f"Redistribution not permitted for {entry.asset_id}")
    if entry.license_spdx.upper() not in ALLOWED_LICENSES:
        errors.append(
            f"Unsupported/unverified license '{entry.license_spdx}' for {entry.asset_id}"
        )
    return (len(errors) == 0, errors)


def detect_duplicates(assets: List[PreparedAsset]) -> int:
    seen: Dict[str, str] = {}
    dup_count = 0
    for asset in assets:
        if asset.sha256 in seen:
            asset.is_duplicate_of = seen[asset.sha256]
            asset.warnings.append(f"Duplicate content of {seen[asset.sha256]}")
            dup_count += 1
        else:
            seen[asset.sha256] = asset.asset_id
    return dup_count
