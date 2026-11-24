"""Khronos glTF Sample Assets (raw.githubusercontent.com, no key). Each model carries its own licence in its LICENSE.md (CC0, CC BY 4.0, ...): the record keeps the exact text hash and flags unknown licences for review."""
import hashlib
import re

from ..common import CACHE_DIR, http_get, now, sha256_file
from .. import extract
from ..catalog import classify

RAW = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models"


def search(query="", limit=40):
    idx = http_get(f"{RAW}/model-index.json", expect_json=True)
    q = query.lower().split()
    out = []
    for m in idx:
        hay = (m["name"] + " " + m.get("label", "") + " " + " ".join(m.get("tags", []))).lower()
        if all(t in hay for t in q):
            out.append({"id": m["name"], "label": m.get("label"), "tags": m.get("tags", []), "variants": list(m.get("variants", {}))})
    return out[:limit]


def _license(text):
    t = text.lower()
    if "cc0" in t or "public domain" in t and "cc by" not in t:
        return "CC0-1.0", "allowed", ""
    m = re.search(r"cc[- ]by(?:[- ](sa|nc|nd))?[- ]?(\d\.\d)?", t)
    if m:
        suffix = (m.group(1) or "").upper()
        ver = m.group(2) or "4.0"
        spdx = "CC-BY" + (f"-{suffix}" if suffix else "") + f"-{ver}"
        if suffix in ("NC", "ND"):
            return spdx, "unknown", "restricted variant: not for the public repository, review before use"
        return spdx, "allowed-with-attribution", ""
    if "sketchfab standard" in t or "all rights" in t:
        return "LicenseRef-restricted", "forbidden", "restricted: do not redistribute"
    return "LicenseRef-unknown", "unknown", "read the model's LICENSE.md"


def fetch(asset_id, variant="glTF-Binary"):
    idx = http_get(f"{RAW}/model-index.json", expect_json=True)
    m = next((x for x in idx if x["name"] == asset_id), None)
    if m is None:
        raise RuntimeError(f"{asset_id} is not in the Khronos index")
    out_dir = CACHE_DIR / "khronos" / asset_id
    fname = m["variants"][variant]
    glb = out_dir / fname
    if not glb.exists():
        http_get(f"{RAW}/{asset_id}/{variant}/{fname}", stream_to=glb, timeout=300)
    lic = out_dir / "LICENSE.md"
    if not lic.exists():
        try:
            http_get(f"{RAW}/{asset_id}/LICENSE.md", stream_to=lic)
        except Exception:
            lic.write_text("")
    text = lic.read_text(errors="ignore")
    spdx, redistribution, notes = _license(text)
    files = [{"path": str(p.relative_to(CACHE_DIR)), "sha256": sha256_file(p), "bytes": p.stat().st_size, "role": "model" if p == glb else "license", "format": p.suffix.lstrip(".")} for p in (glb, lic)]
    tech = extract.describe(glb)
    category, _ = classify("model", m.get("label", asset_id), m.get("tags", []))
    creators = re.findall(r"(?:by|Created by|created by)\s+([^\n,.]+)", text)[:2]
    return {
        "schema": 1, "id": f"model.khronos.{asset_id.lower()}", "kind": "model", "category": category, "name": m.get("label", asset_id), "description": "Khronos glTF sample asset", "tags": m.get("tags", []),
        "source": {"name": "khronos", "source_id": asset_id, "url": f"https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/{asset_id}", "retrieved": now(), "acquire": f"saige-assets fetch khronos {asset_id}"},
        "creators": creators, "license": {"spdx": spdx, "url": f"{RAW}/{asset_id}/LICENSE.md", "text_sha256": hashlib.sha256(text.encode()).hexdigest(), "commercial_use": True if redistribution.startswith("allowed") else "unknown",
                                              "redistribution": redistribution, "attribution": text.strip().splitlines()[0] if text.strip() else "", "ai_training": "unknown", "notes": notes},
        "files": files, "technical": tech, "status": "candidate", "suitability": {"runtime_ready": None, "notes": []}, "version": 1,
    }
