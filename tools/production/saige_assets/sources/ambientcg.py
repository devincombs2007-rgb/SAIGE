"""ambientCG (CC0 PBR materials, HDRIs, some models). API v2: https://docs.ambientcg.com/api/ (no key). Downloads are ZIPs of texture maps."""
import zipfile
from pathlib import Path

from ..common import CACHE_DIR, http_get, now, sha256_file
from .. import extract
from ..catalog import classify

API = "https://ambientcg.com/api/v2/full_json"
LICENSE = {"spdx": "CC0-1.0", "url": "https://docs.ambientcg.com/license/", "commercial_use": True, "redistribution": "allowed", "ai_training": True, "attribution": "", "notes": "ambientCG assets are CC0."}
KIND = {"Material": "material", "Atlas": "material", "Decal": "material", "HDRI": "hdri", "3DModel": "model", "Brush": "texture", "Substance": "material"}


def search(query="", type="Material", limit=20):
    params = f"?type={type}&limit={limit}&sort=Popular&include=tagData,displayData" + (f"&q={query}" if query else "")
    d = http_get(API + params, expect_json=True)
    return [{"id": a["assetId"], "name": a.get("displayName"), "category": a.get("category"), "tags": a.get("tags", [])[:8], "downloads": a.get("downloadCount"), "creation": a.get("creationMethod")} for a in d.get("foundAssets", [])]


def fetch(asset_id, attribute="1K-JPG"):
    d = http_get(f"{API}?id={asset_id}&include=downloadData,tagData,displayData", expect_json=True)
    a = d["foundAssets"][0]
    dl = next((x for x in a["downloadFolders"]["default"]["downloadFiletypeCategories"]["zip"]["downloads"] if x["attribute"] == attribute), None)
    if dl is None:
        raise RuntimeError(f"{asset_id} has no {attribute} download")
    out_dir = CACHE_DIR / "ambientcg" / asset_id / attribute
    zpath = out_dir / (asset_id + "_" + attribute + ".zip")
    if not zpath.exists():
        http_get(dl["fullDownloadPath"], stream_to=zpath, timeout=600)
    with zipfile.ZipFile(zpath) as z:
        z.extractall(out_dir / "files")
    files = []
    for p in sorted((out_dir / "files").rglob("*")):
        if p.is_file() and p.suffix.lower() in (".png", ".jpg", ".exr", ".hdr", ".glb", ".gltf", ".usdc", ".mtlx", ".obj", ".fbx"):
            low = p.name.lower()
            role = "base_color" if "color" in low else "normal_gl" if "normalgl" in low else "normal_dx" if "normaldx" in low else "roughness" if "roughness" in low else "metalness" if "metalness" in low else "ao" if "ambientocclusion" in low else "displacement" if "displacement" in low else "emission" if "emission" in low else "other"
            files.append({"path": str(p.relative_to(CACHE_DIR)), "sha256": sha256_file(p), "bytes": p.stat().st_size, "role": role, "format": p.suffix.lstrip(".")})
    kind = KIND.get(a.get("dataType"), "material")
    base = next((f for f in files if f["role"] in ("base_color",)), files[0])
    tech = extract.describe(CACHE_DIR / base["path"])
    tags = a.get("tags", [])
    category, _ = classify(kind, a.get("displayName", asset_id), tags, [a.get("category", "")])
    return {
        "schema": 1, "id": f"{kind}.ambientcg.{asset_id.lower()}", "kind": kind, "category": category, "name": a.get("displayName", asset_id), "description": a.get("description", "") or "", "tags": tags,
        "source": {"name": "ambientcg", "source_id": asset_id, "url": a.get("shortLink", f"https://ambientcg.com/a/{asset_id}"), "retrieved": now(), "acquire": f"saige-assets fetch ambientcg {asset_id} --res {attribute}"},
        "creators": ["ambientCG / Lennart Demes"], "license": dict(LICENSE, attribution="ambientCG.com (CC0)"), "files": files,
        "technical": dict(tech, maps=[f["role"] for f in files], dimensions_mm=[a.get("dimensionX"), a.get("dimensionY"), a.get("dimensionZ")], creation_method=a.get("creationMethod")),
        "status": "candidate", "suitability": {"runtime_ready": None, "notes": []}, "version": 1,
    }
