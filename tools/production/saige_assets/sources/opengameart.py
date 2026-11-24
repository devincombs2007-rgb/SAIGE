"""OpenGameArt (opengameart.org): no official API, so this reads a content page you name (https://opengameart.org/content/<slug>) and records its files, its licence(s), its author and
a hash of the page text. The licence of each item is its own (CC0, CC-BY, CC-BY-SA, GPL, OGA-BY ...): only CC0 / CC-BY are marked redistributable; everything else is `unknown` for review."""
import html, re
from pathlib import Path
from urllib.parse import unquote, urlparse

from ..common import CACHE_DIR, http_get, now, sha256_file, slug
from .. import extract
from ..catalog import classify

LICENSES = {"zero/1.0": ("CC0-1.0", "allowed"), "by/3.0": ("CC-BY-3.0", "allowed-with-attribution"), "by/4.0": ("CC-BY-4.0", "allowed-with-attribution"), "by-sa/3.0": ("CC-BY-SA-3.0", "unknown"), "by-sa/4.0": ("CC-BY-SA-4.0", "unknown")}


ART_TYPES = {"audio": 12, "3d": 10, "2d": 9, "texture": 14, "music": 12}  # field_art_type_tid of OpenGameArt's art-search (audio covers sound effects and music)


def search(query, art_type="audio", limit=10):
    """Titles and content URLs from OpenGameArt's own search page (no API exists; this reads the result list politely, one request)."""
    from urllib.parse import quote
    page = http_get(f"https://opengameart.org/art-search?keys={quote(query)}&field_art_type_tid%5B%5D={ART_TYPES.get(art_type, 12)}", delay=1.0).decode("utf8", "ignore")
    seen, rows = set(), []
    for m in re.finditer(r'href="(/content/[^"#?]+)"[^>]*>([^<]*)<', page):
        path, title = m.group(1), html.unescape(m.group(2)).strip()
        if path == "/content/faq" or path in seen or not title:
            continue
        seen.add(path)
        rows.append({"url": "https://opengameart.org" + path, "title": title})
    return rows[:limit]


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
    # Packs: unpack and describe the audio inside (duration, loudness, loop seam), so the record says what is in the pack.
    contents = []
    for f in rec_files:
        if f["format"] == "zip":
            import zipfile
            with zipfile.ZipFile(CACHE_DIR / f["path"]) as z:
                z.extractall(out_dir / "contents")
        elif f["format"] == "7z":
            import shutil, subprocess
            exe = shutil.which("7z") or shutil.which("7za") or shutil.which("7zr")
            if exe:
                subprocess.run([exe, "x", "-y", f"-o{out_dir / 'contents'}", str(CACHE_DIR / f["path"])], capture_output=True, check=False)
    if (out_dir / "contents").exists() is False:
        # loose audio files: describe them in place
        for f in rec_files:
            if f["format"] in ("wav", "ogg", "mp3", "flac"):
                d = extract.describe(CACHE_DIR / f["path"])
                contents.append({"file": Path(f["path"]).name, "bytes": f["bytes"], **{k: d[k] for k in ("duration_s", "sample_rate", "channels", "peak_dbfs", "lufs_integrated", "loop_seam_ratio") if k in d}})
    for p in sorted((out_dir / "contents").rglob("*")) if (out_dir / "contents").exists() else []:
        if p.is_file() and p.suffix.lower() in (".wav", ".ogg", ".mp3", ".flac"):
            d = extract.describe(p)
            contents.append({"file": str(p.relative_to(out_dir / "contents")), "bytes": p.stat().st_size, **{k: d[k] for k in ("duration_s", "sample_rate", "channels", "peak_dbfs", "lufs_integrated", "loop_seam_ratio") if k in d}})
        if len(contents) >= 60:
            break
    if contents:
        tech = dict(tech, pack_contents=contents, pack_files=len(contents), pack_seconds=round(sum(c.get("duration_s", 0) for c in contents), 1))
    category, _ = classify(kind, title, [])
    return {
        "schema": 1, "id": f"{kind}.opengameart.{asset_id}", "kind": kind, "category": category, "name": title, "description": "", "tags": [],
        "source": {"name": "opengameart", "source_id": asset_id, "url": url, "retrieved": now(), "acquire": f"saige-assets fetch opengameart {url} --kind {kind}"},
        "creators": [author], "license": {"spdx": spdx, "url": lic_links[0][0] if lic_links else "", "commercial_use": True if redist.startswith("allowed") else "unknown", "redistribution": redist, "attribution": f"{title} by {author} (OpenGameArt.org)", "ai_training": "unknown", "notes": "" if redist != "unknown" else "licence needs review"},
        "files": rec_files, "technical": tech, "status": "candidate", "suitability": {"runtime_ready": None, "notes": []}, "version": 1,
    }
