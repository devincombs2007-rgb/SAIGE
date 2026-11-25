"""MakeHuman community assets (files.makehumancommunity.org, no key): about 3,400 clothes, hair-and-eyes materials, skins, poses, rigs, expressions, targets and models, each with its own licence in
the public asset index (CC0, CC-BY or AGPL). `search` reads the index; `fetch` downloads one asset's files (mhclo/mhmat/obj/png ...) and records its licence exactly. The assets are MakeHuman formats:
turning them into a rigged, clothed glTF character needs the MakeHuman application or its Blender add-on MPFB, which is not run here (see docs/assets/integration-report.md).
Licences: only CC0 and CC-BY are marked redistributable; AGPL assets are `unknown` (copyleft on the asset needs a decision before the public repository takes it)."""
from pathlib import Path
from urllib.parse import urlparse

from ..common import CACHE_DIR, http_get, now, sha256_file, slug
from ..catalog import classify

INDEX = "https://files.makehumancommunity.org/sites/default/files/assets.json"
KIND = {"clothes": "model", "model": "model", "proxy": "model", "rig": "model", "skin": "material", "material": "material", "pose": "animation", "expression": "animation", "target": "other"}


def _index():
    return http_get(INDEX, expect_json=True)


def search(query="", type=None, license=None, limit=30):
    q = query.lower().split()
    rows = []
    for nid, a in _index().items():
        if type and a.get("type") != type:
            continue
        if license and a.get("license") != license:
            continue
        hay = " ".join(str(a.get(k, "")) for k in ("title", "category", "description", "username")).lower()
        if all(t in hay for t in q):
            rows.append({"id": nid, "title": a.get("title"), "type": a.get("type"), "category": a.get("category"), "license": a.get("license"), "author": a.get("username")})
    return rows[:limit]


def fetch(nid):
    a = _index()[str(nid)]
    lic = a.get("license")
    spdx = {"CC0": "CC0-1.0", "CC-BY": "CC-BY-4.0", "AGPL": "AGPL-3.0-only"}.get(lic, "LicenseRef-unknown")
    redistribution = {"CC0": "allowed", "CC-BY": "allowed-with-attribution"}.get(lic, "unknown")
    out_dir = CACHE_DIR / "makehuman" / str(nid)
    files = []
    for role, url in a.get("files", {}).items():
        if role in ("thumb",):
            continue
        dest = out_dir / Path(urlparse(url).path).name
        if not dest.exists():
            http_get(url.replace("http://", "https://"), stream_to=dest, delay=0.5)
        files.append({"path": str(dest.relative_to(CACHE_DIR)), "sha256": sha256_file(dest), "bytes": dest.stat().st_size, "role": role, "format": dest.suffix.lstrip(".")})
    kind = KIND.get(a.get("type"), "other")
    category = "character.clothing" if a.get("type") == "clothes" else "character.humanoid" if a.get("type") in ("model", "rig", "proxy") else classify("material" if kind == "material" else kind, a.get("title", ""), [])[0]
    if a.get("type") == "pose" or a.get("type") == "expression":
        category = "animation.skeletal"
    return {
        "schema": 1, "id": f"{kind}.makehuman.{slug(a.get('title', str(nid)))}_{nid}", "kind": kind, "category": category, "name": a.get("title", str(nid)), "description": a.get("description", ""), "tags": [a.get("type", ""), a.get("category", "")],
        "source": {"name": "makehuman", "source_id": str(nid), "url": f"https://files.makehumancommunity.org/sites/default/files/assets.json#{nid}", "retrieved": now(), "acquire": f"saige-assets fetch makehuman {nid}"},
        "creators": [a.get("username", "")], "license": {"spdx": spdx, "url": "https://static.makehumancommunity.org/assets/license.html", "commercial_use": True if redistribution.startswith("allowed") else "unknown", "redistribution": redistribution,
                                                        "attribution": f"{a.get('title')} by {a.get('username')} (MakeHuman community assets, {lic})" if lic != "CC0" else "", "ai_training": "unknown", "notes": "MakeHuman asset format" + ("" if redistribution != "unknown" else "; licence needs review")},
        "files": files, "technical": {"makehuman_type": a.get("type"), "makehuman_category": a.get("category"), "compatibility": a.get("compatibility")}, "status": "candidate", "suitability": {"runtime_ready": False, "notes": ["MakeHuman format: needs conversion (MakeHuman or MPFB) before the engine can import it"]}, "version": 1,
    }
