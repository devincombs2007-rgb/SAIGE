"""OpenGameArt (opengameart.org): no official API, so this reads a content page you name (https://opengameart.org/content/<slug>) and records its files, its licence(s), its author and
a hash of the page text. The licence of each item is its own (CC0, CC-BY, CC-BY-SA, GPL, OGA-BY ...): only CC0 / CC-BY are marked redistributable; everything else is `unknown` for review."""
import html, re
from pathlib import Path
from urllib.parse import unquote, urlparse

from ..common import CACHE_DIR, http_get, now, sha256_file, slug
from .. import extract
from ..catalog import classify

LICENSES = {"zero/1.0": ("CC0-1.0", "allowed"), "by/3.0": ("CC-BY-3.0", "allowed-with-attribution"), "by/4.0": ("CC-BY-4.0", "allowed-with-attribution"), "by-sa/3.0": ("CC-BY-SA-3.0", "unknown"), "by-sa/4.0": ("CC-BY-SA-4.0", "unknown")}


def fetch(url, kind="model"):
    page = http_get(url, delay=1.0).decode("utf8", "ignore")
    title = html.unescape((re.search(r"<title>([^<|]+)", page) or [None, url])[1]).strip()
    lic_links = re.findall(r"href='(https?://creativecommons\.org/(?:publicdomain|licenses)/([^']+?)/?)'", page)
    spdx, redist = "LicenseRef-unknown", "unknown"
    for _, code in lic_links:
        key = code.replace("publicdomain/", "").replace("licenses/", "")
        if key in LICENSES:
            spdx, redist = LICENSES[key]
            break
        elif key.startswith("by/") or key.startswith("zero/"):
            spdx, redist = LICENSES.get(key, ("CC-" + key.upper().replace("/", "-"), "unknown"))
    if "OGA-BY" in page:
        spdx, redist = "LicenseRef-OGA-BY-3.0", "allowed-with-attribution"
    author = (re.search(r'href="/users/([^"]+)"', page) or [None, "unknown"])[1]
    files = sorted(set(re.findall(r'href="(https://opengameart.org/sites/default/files/[^"]+\.(?:zip|wav|ogg|mp3|flac|png|glb|gltf|obj|fbx|blend|7z))"', page)))
    files = [f for f in files if "/styles/" not in f]
    asset_id = slug(urlparse(url).path.split("/")[-1])
    out_dir = CACHE_DIR / "opengameart" / asset_id
    rec_files = []
    for f in files[:12]:
        dest = out_dir / unquote(Path(urlparse(f).path).name)
        if not dest.exists():
            http_get(f, stream_to=dest, timeout=300, delay=1.0)
        rec_files.append({"path": str(dest.relative_to(CACHE_DIR)), "sha256": sha256_file(dest), "bytes": dest.stat().st_size, "role": "content", "format": dest.suffix.lstrip(".")})
    if not rec_files:
        raise RuntimeError("no downloadable files were found on the page")
    first = CACHE_DIR / rec_files[0]["path"]
    tech = extract.describe(first) if first.suffix.lower() != ".zip" else {"format": "zip"}
    category, _ = classify(kind, title, [])
    return {
        "schema": 1, "id": f"{kind}.opengameart.{asset_id}", "kind": kind, "category": category, "name": title, "description": "", "tags": [],
        "source": {"name": "opengameart", "source_id": asset_id, "url": url, "retrieved": now(), "acquire": f"saige-assets fetch opengameart {url} --kind {kind}"},
        "creators": [author], "license": {"spdx": spdx, "url": lic_links[0][0] if lic_links else "", "commercial_use": True if redist.startswith("allowed") else "unknown", "redistribution": redist, "attribution": f"{title} by {author} (OpenGameArt.org)", "ai_training": "unknown", "notes": "" if redist != "unknown" else "licence needs review"},
        "files": rec_files, "technical": tech, "status": "candidate", "suitability": {"runtime_ready": None, "notes": []}, "version": 1,
    }
