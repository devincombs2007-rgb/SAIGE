#!/usr/bin/env python3
"""Showcase 04 asset restoration: reproduces, byte for byte, the third-party files the renderer reads, so the visual regression tests can run on a machine that has nothing.

  restore_assets.py --restore DIR      download whatever is missing or wrong into DIR (the cache root: DIR/polyhaven/..., DIR/khronos/...) and verify every file's SHA-256 against assets.lock.json
  restore_assets.py --verify DIR       only verify (no network); exit 1 when anything is missing or different
  restore_assets.py --make-lock DIR    regenerate assets.lock.json from the files the renderer actually opens (see "How the list was made") taking hashes from DIR; for maintainers

The lock lists 149 files (39 Poly Haven folders, CC0, and the Khronos "Car Concept", CC BY 4.0: its attribution is on the film's end card and in docs/showcase/04-world-alive.md): about 87 MB.
Nothing is committed but the lock: the files are the licensors', fetched from their own servers (Poly Haven's documented API and dl.polyhaven.org, the Khronos sample-asset repository on
raw.githubusercontent.com). A hash that does not match is an ERROR and the file is removed: an upstream change must be reviewed and the lock regenerated, never accepted silently, and a missing
asset never turns a test into a skipped one when SAIGE_REQUIRE_VISUAL=1 (see visual_regression.py).
The humanoid animation library (the UAL glb the film's residents use) is restored by tools/animation/fetch_humanoid_assets.py, as before.

How the list was made: the renderer was run under `strace -f -e trace=openat` for one still; every file it opened under the cache was recorded with its SHA-256.
"""
import argparse
import hashlib
import json
import os
import sys
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOCK = HERE.parent / "assets.lock.json"
API = "https://api.polyhaven.com"
CAR_URL = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/CarConcept/glTF-Binary/CarConcept.glb"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def get(url, tries=4, as_json=False):
    last = None
    for attempt in range(tries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "saige-asset-restore"})
            with urllib.request.urlopen(req, timeout=120) as r:
                data = r.read()
            return json.loads(data) if as_json else data
        except Exception as e:  # noqa: BLE001 - any network error is retried, then reported
            last = e
            time.sleep(2 * (attempt + 1))
    raise RuntimeError("could not download %s: %s" % (url, last))


def polyhaven_urls(asset_id):
    """Map from a path relative to DIR/polyhaven/<id>/1k/ to its download URL (a model's files, or a material's three maps)."""
    files = get("%s/files/%s" % (API, asset_id), as_json=True)
    urls = {}
    if "gltf" in files and "1k" in files["gltf"]:
        node = files["gltf"]["1k"]["gltf"]
        urls["%s_1k.gltf" % asset_id] = node["url"]
        for rel, inc in node.get("include", {}).items():
            urls[rel] = inc["url"]
    for key in ("Diffuse", "arm", "nor_gl"):
        if key in files and "1k" in files[key]:
            n = files[key]["1k"]
            n = n["jpg"] if "jpg" in n else next(iter(n.values()))
            urls[n["url"].rsplit("/", 1)[-1]] = n["url"]   # the file's own name: some materials say "diffuse", others "diff"
    return urls


def verify(root, lock):
    bad = []
    for e in lock["files"]:
        p = root / e["path"]
        if not p.is_file():
            bad.append((e["path"], "missing"))
        elif sha256(p) != e["sha256"]:
            bad.append((e["path"], "hash differs"))
    return bad


def restore(root, lock):
    todo = [e for e in lock["files"] if not (root / e["path"]).is_file() or sha256(root / e["path"]) != e["sha256"]]
    print("restoring %d of %d files into %s" % (len(todo), len(lock["files"]), root))
    by_asset = {}
    for e in todo:
        parts = e["path"].split("/")
        by_asset.setdefault(parts[0] + "/" + parts[1], []).append(e)
    cache = {}
    for asset, entries in sorted(by_asset.items()):
        for e in entries:
            dest = root / e["path"]
            parts = e["path"].split("/")
            if parts[0] == "khronos":
                url = CAR_URL
            else:
                aid = parts[1]
                if aid not in cache:
                    cache[aid] = polyhaven_urls(aid)
                rel = "/".join(parts[3:])   # polyhaven/<id>/1k/<rel>
                url = cache[aid].get(rel) or cache[aid].get(parts[-1])
                if url is None:
                    print("  no source for %s" % e["path"])
                    return 1
            data = get(url)
            if hashlib.sha256(data).hexdigest() != e["sha256"]:
                print("  HASH MISMATCH for %s (from %s): the upstream file changed; review it and regenerate the lock" % (e["path"], url))
                return 1
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        print("  %s: %d file(s) ok" % (asset, len(entries)))
    bad = verify(root, lock)
    for p, why in bad:
        print("  STILL BAD: %s (%s)" % (p, why))
    return 1 if bad else 0


def make_lock(root, listing):
    files = []
    for line in Path(listing).read_text().splitlines():
        p = Path(line.strip())
        if not p.is_file():
            continue
        rel = p.relative_to(root).as_posix()
        if not (rel.startswith("polyhaven/") or rel.startswith("khronos/")):
            continue
        files.append({"path": rel, "sha256": sha256(p), "bytes": p.stat().st_size})
    files.sort(key=lambda e: e["path"])
    lock = {
        "description": "Third-party files read by the Showcase 04 renderer (saige_alive), restored by tools/restore_assets.py. Resolution 1k.",
        "licenses": {
            "polyhaven": {"spdx": "CC0-1.0", "url": "https://polyhaven.com/license"},
            "khronos/CarConcept": {"spdx": "CC-BY-4.0", "attribution": "Car Concept (c) Khronos Group, created by Eric Chadwick of Darmstadt Graphics Group GmbH, 2024",
                                   "url": "https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/Models/CarConcept/LICENSE.md"},
        },
        "files": files,
    }
    LOCK.write_text(json.dumps(lock, indent=1) + "\n")
    print("wrote %s: %d files, %.1f MB" % (LOCK, len(files), sum(f["bytes"] for f in files) / 1e6))
    return 0


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--restore", metavar="DIR")
    g.add_argument("--verify", metavar="DIR")
    g.add_argument("--make-lock", metavar="DIR")
    ap.add_argument("--listing", help="with --make-lock: a file with one absolute path per line (the strace list)")
    args = ap.parse_args()
    if args.make_lock:
        if not args.listing:
            ap.error("--make-lock needs --listing")
        return make_lock(Path(args.make_lock), args.listing)
    lock = json.loads(LOCK.read_text())
    if args.verify:
        bad = verify(Path(args.verify), lock)
        for p, why in bad:
            print("%s: %s" % (p, why))
        print("verify: %s" % ("ok, %d files" % len(lock["files"]) if not bad else "%d problem(s)" % len(bad)))
        return 1 if bad else 0
    return restore(Path(args.restore), lock)


if __name__ == "__main__":
    sys.exit(main())
