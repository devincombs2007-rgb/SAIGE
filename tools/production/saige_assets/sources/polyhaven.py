"""Poly Haven (CC0). API: https://api.polyhaven.com (documented, no key). Models are glTF with textures; materials are PBR texture sets; HDRIs are .hdr/.exr."""
import shutil
from pathlib import Path, PurePosixPath
from urllib.parse import urlsplit

from ..common import CACHE_DIR, http_get, now, sha256_file
from .. import safety
from .. import extract
from ..catalog import classify

API = "https://api.polyhaven.com"
HOSTS = ("api.polyhaven.com", "dl.polyhaven.org")  # the API and its file host; redirects elsewhere are refused
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
    """(url, size, md5, role, relative path) for every file the asset needs: a model's glTF and every buffer and texture it includes (its
    `include` map, so the glTF is complete), an HDRI, or a material's maps. A missing resolution or format is an error, not a silent gap."""
    picks = []
    if type == "models":
        node = files["gltf"][res]["gltf"]
        picks.append((node["url"], node["size"], node.get("md5"), "model", PurePosixPath(urlsplit(node["url"]).path).name))
        for rel, inc in node.get("include", {}).items():
            picks.append((inc["url"], inc["size"], inc.get("md5"), "dependency", rel))
    elif type == "hdris":
        node = files["hdri"][res][fmt]
        picks.append((node["url"], node["size"], node.get("md5"), "hdri", PurePosixPath(urlsplit(node["url"]).path).name))
    else:  # textures: diffuse, arm (AO/rough/metal), normal GL, displacement and roughness where published
        for key, role in (("Diffuse", "base_color"), ("arm", "orm"), ("nor_gl", "normal"), ("Rough", "roughness"), ("Displacement", "displacement")):
            if key in files and res in files[key]:
                n = files[key][res]["jpg"] if "jpg" in files[key][res] else next(iter(files[key][res].values()))
                picks.append((n["url"], n["size"], n.get("md5"), role, PurePosixPath(urlsplit(n["url"]).path).name))
        if not any(p[3] == "base_color" for p in picks):
            raise RuntimeError(f"no {res} base colour map")
    return picks


def fetch(asset_id, type="models", res="1k", fmt="hdr"):
    safety.contained(CACHE_DIR / "polyhaven", asset_id)  # the asset id becomes a directory name
    info = http_get(f"{API}/info/{asset_id}", expect_json=True, allowed_hosts=HOSTS)
    files = http_get(f"{API}/files/{asset_id}", expect_json=True, allowed_hosts=HOSTS)
    kind = TYPES[type]
    out_dir = CACHE_DIR / "polyhaven" / asset_id / res
    rec_files = []
    for url, size, md5, role, rel in _pick_files(files, type, res, fmt):
        dest = safety.contained(out_dir, rel)  # a key like "../../x" from the API is refused, not written outside the cache
        http_get(url, stream_to=dest, timeout=300, allowed_hosts=HOSTS, expected_size=size, expected_md5=md5)
        rec_files.append({"path": str(dest.relative_to(CACHE_DIR.resolve())), "sha256": sha256_file(dest), "bytes": dest.stat().st_size, "role": role, "format": dest.suffix.lstrip("."),
                          "source_url": url, "source_md5": md5})
    main = next(f for f in rec_files if f["role"] in ("model", "hdri", "base_color"))
    if kind == "model":
        missing = safety.missing_gltf_dependencies(CACHE_DIR / main["path"])
        if missing:  # every buffer and texture the glTF names must have arrived, inside its folder
            raise RuntimeError(f"{asset_id}: the glTF references files that were not downloaded: {missing}")
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
