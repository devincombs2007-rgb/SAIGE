"""Poly Haven (CC0). API: https://api.polyhaven.com (documented, no key). Models are glTF with textures; materials are PBR texture sets; HDRIs are .hdr/.exr."""
import shutil
from pathlib import Path

from ..common import CACHE_DIR, http_get, now, sha256_file
from .. import extract
from ..catalog import classify

API = "https://api.polyhaven.com"
LICENSE = {"spdx": "CC0-1.0", "url": "https://polyhaven.com/license", "commercial_use": True, "redistribution": "allowed", "ai_training": True, "attribution": "", "notes": "Poly Haven assets are CC0; credit is appreciated, not required."}
CATEGORY_MAP = [  # Poly Haven's own categories, in priority order, to the SAIGE taxonomy
    ("seating", "furniture.household"), ("furniture", "furniture.household"), ("plants", "vegetation"), ("trees", "vegetation"), ("rocks", "terrain.landscape"), ("terrain", "terrain.landscape"),
    ("vehicles", "vehicle.car"), ("buildings", "building.exterior"), ("architectural", "building.exterior"), ("containers", "prop.environment"), ("industrial", "prop.environment"), ("props", "prop.environment"),
]
TYPES = {"hdris": "hdri", "textures": "material", "models": "model"}


def search(query="", type="models", category=None, limit=20):
    data = http_get(f"{API}/assets?t={type}" + (f"&c={category}" if category else ""), expect_json=True)
    q = query.lower().split()
    out = []
    for aid, a in data.items():
        hay = " ".join([aid, a.get("name", ""), " ".join(a.get("tags", [])), " ".join(a.get("categories", []))]).lower()
        if all(t in hay for t in q):
            out.append({"id": aid, "name": a.get("name"), "polycount": a.get("polycount"), "categories": a.get("categories"), "authors": list(a.get("authors", {})), "max_resolution": a.get("max_resolution"), "downloads": a.get("download_count")})
    out.sort(key=lambda x: -(x["downloads"] or 0))
    return out[:limit]


def _pick_files(files, type, res, fmt):
    picks = []
    if type == "models":
        node = files["gltf"][res]["gltf"]
        picks.append((node["url"], node["size"], "model"))
        for rel, inc in node.get("include", {}).items():
            picks.append((inc["url"], inc["size"], "dependency", rel))
    elif type == "hdris":
        node = files["hdri"][res][fmt]
        picks.append((node["url"], node["size"], "hdri"))
    else:  # textures: diffuse, arm (AO/rough/metal), normal GL
        for key, role in (("Diffuse", "base_color"), ("arm", "orm"), ("nor_gl", "normal")):
            if key in files and res in files[key]:
                n = files[key][res]["jpg"] if "jpg" in files[key][res] else next(iter(files[key][res].values()))
                picks.append((n["url"], n["size"], role))
    return picks


def fetch(asset_id, type="models", res="1k", fmt="hdr"):
    info = http_get(f"{API}/info/{asset_id}", expect_json=True)
    files = http_get(f"{API}/files/{asset_id}", expect_json=True)
    kind = TYPES[type]
    out_dir = CACHE_DIR / "polyhaven" / asset_id / res
    rec_files = []
    for pick in _pick_files(files, type, res, fmt):
        url, size, role = pick[0], pick[1], pick[2]
        rel = pick[3] if len(pick) > 3 else Path(url).name
        dest = out_dir / rel
        if not dest.exists():
            http_get(url, stream_to=dest, timeout=300)
        rec_files.append({"path": str(dest.relative_to(CACHE_DIR)), "sha256": sha256_file(dest), "bytes": dest.stat().st_size, "role": role, "format": dest.suffix.lstrip(".")})
    main = next(f for f in rec_files if f["role"] in ("model", "hdri", "base_color"))
    tech = extract.describe(CACHE_DIR / main["path"])
    if kind == "model":
        # the model's total GPU footprint includes the sidecar textures
        tex = 0
        for f in rec_files:
            if f["format"] in ("jpg", "png"):
                d = extract.describe(CACHE_DIR / f["path"])
                tex += d.get("gpu_bytes_rgba8_mips", 0)
        tech["texture_gpu_bytes"] = tex
    tags = list(info.get("tags", []))
    cats = list(info.get("categories", []))
    category, _ = classify(kind, info.get("name", asset_id), tags, cats)
    if kind == "model":
        for key, mapped in CATEGORY_MAP:
            if key in [c.lower() for c in cats] or any(key in str(c).lower() for c in [info.get("category", "")]):
                category = mapped
                break
        if any(w in asset_id for w in ("lamp", "barrier", "utility", "traffic", "sign", "fence")) and category in ("prop.environment", "furniture.household"):
            category = "infrastructure.road" if "desk" not in asset_id and "ceiling" not in asset_id else category
    authors = list(info.get("authors", {}))
    return {
        "schema": 1, "id": f"{kind}.polyhaven.{asset_id.lower()}", "kind": kind, "category": category, "name": info.get("name", asset_id), "description": info.get("description", ""), "tags": tags + cats,
        "source": {"name": "polyhaven", "source_id": asset_id, "url": f"https://polyhaven.com/a/{asset_id}", "retrieved": now(), "acquire": f"saige-assets fetch polyhaven {asset_id} --type {type} --res {res}"},
        "creators": authors, "license": dict(LICENSE, attribution="; ".join(authors)), "files": rec_files, "technical": dict(tech, polycount_source=info.get("polycount"), dimensions_mm=info.get("dimensions"), max_resolution=info.get("max_resolution")),
        "status": "candidate", "suitability": {"runtime_ready": None, "notes": []}, "version": 1,
    }
