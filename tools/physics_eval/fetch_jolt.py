#!/usr/bin/env python3
"""Fetches Jolt Physics (MIT licence) at a pinned tag into DIR for the isolated physics evaluation prototype; nothing is committed to the engine.

  python3 tools/physics_eval/fetch_jolt.py DIR [--tag v5.3.0] [--update-manifest]

Why this and not git: github.com itself is not reachable from the cloud session's network policy, but raw.githubusercontent.com is. The file list is Jolt's own `Jolt/Jolt.cmake` (every source and header
of the library), each file is downloaded from the raw host at the tag, and each is verified against `jolt-<tag>.sha256` (written with --update-manifest the first time a tag is used, then checked on every fetch).
Also fetched: LICENSE and the License/Credits files, so the licence travels with the source. DIR/Jolt is then an include root (`#include <Jolt/Jolt.h>` with `-I DIR`).
"""
import argparse, concurrent.futures, hashlib, re, sys, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
RAW = "https://raw.githubusercontent.com/jrouwe/JoltPhysics/{tag}/{path}"


def get(url, retries=4):
    last = None
    for _ in range(retries):
        try:
            with urllib.request.urlopen(url, timeout=60) as r:
                return r.read()
        except Exception as e:  # network hiccup: retry
            last = e
    raise RuntimeError(f"{url}: {last}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--tag", default="v5.3.0")
    ap.add_argument("--update-manifest", action="store_true")
    args = ap.parse_args()
    out = Path(args.dir)
    cmake = get(RAW.format(tag=args.tag, path="Jolt/Jolt.cmake")).decode()
    # Only real source entries: a path of word characters and slashes ending in a source extension (the file also mentions the root inside generator expressions).
    files = sorted(set(re.findall(r"\$\{JOLT_PHYSICS_ROOT\}/([\w/]+\.(?:h|cpp|inl))\b", cmake)))
    files = [f"Jolt/{f}" for f in files if not f.endswith(".natvis")] + ["Jolt/Jolt.cmake", "LICENSE"]
    manifest_path = HERE / f"jolt-{args.tag}.sha256"
    known = {}
    if manifest_path.exists() and not args.update_manifest:
        for line in manifest_path.read_text().splitlines():
            h, _, p = line.partition("  ")
            known[p] = h

    def fetch(path):
        dest = out / path
        if dest.exists() and (not known or hashlib.sha256(dest.read_bytes()).hexdigest() == known.get(path)):
            return path, hashlib.sha256(dest.read_bytes()).hexdigest()
        data = get(RAW.format(tag=args.tag, path=path))
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        return path, hashlib.sha256(data).hexdigest()

    with concurrent.futures.ThreadPoolExecutor(max_workers=12) as ex:
        results = dict(ex.map(fetch, files))
    if args.update_manifest or not manifest_path.exists():
        manifest_path.write_text("".join(f"{h}  {p}\n" for p, h in sorted(results.items())))
        print(f"wrote {manifest_path.name} ({len(results)} files)")
        return 0
    bad = [p for p, h in results.items() if known.get(p) != h]
    for p in bad:
        print("HASH MISMATCH or unlisted:", p)
    print(f"{len(results)} files, {len(bad)} problems")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
