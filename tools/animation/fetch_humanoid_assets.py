#!/usr/bin/env python3
"""Fetches the humanoid animation assets the saige::animation tests and examples use, into DIR, and checks them against tools/animation/assets.json.

  python3 tools/animation/fetch_humanoid_assets.py DIR [--ual PATH_TO_AnimationLibrary_Godot_Standard.glb]
  SAIGE_ANIM_ASSETS=DIR ctest -R animation_assets

DIR/cmu/*.bvh   six CMU motion capture clips (walk, brisk walk, jogs, runs), downloaded from the public conversion on raw.githubusercontent.com (no account, no key) and verified by SHA-256.
DIR/ual/...glb  Quaternius' Universal Animation Library (CC0): the rig with a mesh and the idle, walk, sprint and jump clips. OpenGameArt serves it as a zip behind a page; fetch it once with
                `tools/production/saige-assets fetch opengameart https://opengameart.org/content/universal-animation-library --kind animation` (it lands in the asset cache) and this script
                copies the .glb from there, or pass --ual with the file's path.
Nothing is committed: the CMU files carry no explicit redistribution statement and the library is large. Licenses and the attribution sentence CMU asks for are in assets.json and the catalog.
"""
import argparse, hashlib, json, shutil, sys, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def find_ual_in_cache(name):
    candidates = []
    try:
        sys.path.insert(0, str(HERE.parent / "production"))
        from saige_assets.common import CACHE_DIR  # type: ignore
        candidates.append(Path(CACHE_DIR))
    except Exception:  # the production tools are optional here
        pass
    candidates += [Path("/tmp/saige-cache"), Path.home() / ".saige-cache"]
    for root in candidates:
        for hit in root.rglob(name) if root.exists() else []:
            return hit
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--ual")
    args = ap.parse_args()
    manifest = json.loads((HERE / "assets.json").read_text())
    out = Path(args.dir)
    ok = True
    (out / "cmu").mkdir(parents=True, exist_ok=True)
    for f in manifest["cmu"]["files"]:
        dest = out / "cmu" / f"{f['id']}.bvh"
        if not dest.exists() or sha256(dest) != f["sha256"]:
            print(f"fetching {f['id']} ({f['what']})")
            with urllib.request.urlopen(f["url"], timeout=60) as r:
                dest.write_bytes(r.read())
        good = sha256(dest) == f["sha256"]
        ok = ok and good
        print(f"  {dest.name}: {'ok' if good else 'HASH MISMATCH'}")
    u = manifest["ual"]
    dest = out / "ual" / u["file"]
    (out / "ual").mkdir(parents=True, exist_ok=True)
    src = Path(args.ual) if args.ual else find_ual_in_cache(u["file"])
    if not dest.exists() and src is not None and src.exists():
        shutil.copyfile(src, dest)
    if dest.exists():
        good = sha256(dest) == u["sha256"]
        ok = ok and good
        print(f"  {dest.name}: {'ok' if good else 'HASH MISMATCH'}")
    else:
        ok = False
        print(f"  {u['file']}: missing. Fetch it with: tools/production/saige-assets fetch opengameart {u['source']} --kind animation   then re-run this script, or pass --ual PATH")
    print("attribution (CMU): " + manifest["cmu"]["attribution"])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
