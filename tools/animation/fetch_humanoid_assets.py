#!/usr/bin/env python3
"""Fetches the humanoid animation assets the saige::animation tests and examples use, into DIR, and checks them against tools/animation/assets.json.

  python3 tools/animation/fetch_humanoid_assets.py DIR [--ual PATH_TO_AnimationLibrary_Godot_Standard.glb]
  SAIGE_ANIM_ASSETS=DIR ctest -R animation_assets

DIR/cmu/*.bvh   six CMU motion capture clips (walk, brisk walk, jogs, runs), downloaded from the public conversion on raw.githubusercontent.com (no account, no key) and verified by SHA-256.
DIR/ual/...glb  Quaternius' Universal Animation Library (CC0): the rig with a mesh and the idle, walk, sprint and jump clips. DIR/ual2/...glb the second library (optional: the climb).
                Each is downloaded as the zip OpenGameArt serves (the URL, the zip's SHA-256 and the member to take are in assets.json; the zip's hash is the one in the asset catalog), the member is
                extracted and its own SHA-256 checked. Nothing here needs an account or a key, and only the standard library is used, so a CI runner can restore everything from nothing:
                  python3 tools/animation/fetch_humanoid_assets.py DIR
                A copy already in the asset cache (tools/production/saige-assets) or given with --ual is used first. A hash that does not match is an error and the file is removed.
Nothing is committed: the CMU files carry no explicit redistribution statement and the library is large. Licenses and the attribution sentence CMU asks for are in assets.json and the catalog.
"""
import argparse, hashlib, io, json, shutil, sys, time, urllib.request, zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def download(url, tries=4):
    """The bytes at url, retrying with a growing pause (a CI runner's network is not perfect)."""
    last = None
    for attempt in range(tries):
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                return r.read()
        except Exception as e:  # noqa: BLE001 - any network error is retried, then reported
            last = e
            time.sleep(2 * (attempt + 1))
    raise RuntimeError(f"could not download {url}: {last}")


def restore_from_archive(entry, dest):
    """Downloads entry['archive'] (a zip), checks its SHA-256, extracts the member to dest and checks the member's SHA-256. Returns True when dest is good."""
    a = entry.get("archive")
    if not a:
        return False
    print(f"  downloading {a['url']}")
    blob = download(a["url"])
    if hashlib.sha256(blob).hexdigest() != a["sha256"]:
        print(f"  archive HASH MISMATCH for {a['url']}")
        return False
    with zipfile.ZipFile(io.BytesIO(blob)) as z:
        data = z.read(a["member"])
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(data)
    if sha256(dest) != entry["sha256"]:
        dest.unlink()
        print(f"  extracted {dest.name} HASH MISMATCH")
        return False
    return True


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
    ap.add_argument("--no-cache", action="store_true", help="never use a copy from the asset cache: download and verify everything (what CI does)")
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
    src = Path(args.ual) if args.ual else (None if args.no_cache else find_ual_in_cache(u["file"]))
    if not dest.exists() and src is not None and src.exists():
        shutil.copyfile(src, dest)
    if dest.exists() and sha256(dest) != u["sha256"]:
        dest.unlink()  # a stale or damaged copy: restore it below
    if not dest.exists():
        restore_from_archive(u, dest)
    if dest.exists():
        good = sha256(dest) == u["sha256"]
        ok = ok and good
        print(f"  {dest.name}: {'ok' if good else 'HASH MISMATCH'}")
    else:
        ok = False
        print(f"  {u['file']}: missing and could not be restored from its archive; fetch it with tools/production/saige-assets fetch opengameart {u['source']} --kind animation and re-run, or pass --ual PATH")
    # UAL 2 is optional (the climb test uses it): copied from the asset cache when it is there, never a failure when it is not.
    u2 = manifest.get("ual2")
    if u2 is not None:
        dest2 = out / "ual2" / u2["file"]
        src2 = None if args.no_cache else find_ual_in_cache(u2["file"])
        if not dest2.exists() and src2 is not None and src2.exists():
            (out / "ual2").mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src2, dest2)
        if dest2.exists() and sha256(dest2) != u2["sha256"]:
            dest2.unlink()
        if not dest2.exists():
            restore_from_archive(u2, dest2)
        if dest2.exists():
            good = sha256(dest2) == u2["sha256"]
            ok = ok and good
            print(f"  {dest2.name}: {'ok' if good else 'HASH MISMATCH'} (optional)")
        else:
            print(f"  {u2['file']}: not available (optional; fetch with tools/production/saige-assets fetch opengameart {u2['source']} --kind animation)")
    print("attribution (CMU): " + manifest["cmu"]["attribution"])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
