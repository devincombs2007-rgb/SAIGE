#!/usr/bin/env python3
"""Showcase 04 fixture assets: deterministic stand-ins for the licensed third-party textures, so the visual regression can run in hosted CI without downloading anything and with nothing that can drift.

  make_fixtures.py DIR     writes DIR/polyhaven/<folder>/1k/<name>.jpg for every texture in assets.lock.json (126 files, 64x64, a few KB each), the same relative paths the renderer opens.

The files are PNG data under the .jpg names the renderer asks for (the image decoder reads the format from the bytes, not from the extension). Content depends only on the file's path:
  *_diff_*    a colour fixed by the folder name (distinct per material) with an 8-pixel checker and a diagonal ramp, so that two different surfaces never look alike and a surface that is drawn twice
              (coplanar) shows as the checker flickering;
  *_nor_gl_*  a flat tangent-space normal (128, 128, 255): NO normal-map detail;
  *_arm_*     ambient occlusion 1, roughness 0.67, metallic 0: NO per-pixel material variation;
  *_rough_*   a flat roughness.
What a test on these fixtures can and cannot say is listed in docs/showcase/04-defect-register.md (infrastructure section): it exercises the geometry, depth, shadow and director paths with real materials' colours
replaced by fixed ones; it does NOT cover the third-party textures' detail, the car model, the props (hydrant, bench, bin, lamp, tables, chairs, plants, sofa, crates, planters) or the residents.
Nothing here is licensed material: the files are generated from the lock's file names.
"""
import hashlib
import json
import os
import struct
import sys
import zlib
from pathlib import Path

LOCK = Path(__file__).resolve().parent.parent / "assets.lock.json"
SIZE = 64


def png(width, height, rows):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


def folder_colour(folder):
    h = hashlib.sha256(folder.encode()).digest()
    return [60 + h[0] % 150, 60 + h[1] % 150, 60 + h[2] % 150]


def image_for(path):
    folder = path.split("/")[1]
    name = path.rsplit("/", 1)[-1]
    rows = []
    for y in range(SIZE):
        row = []
        for x in range(SIZE):
            if "_diff_" in name:
                base = folder_colour(folder)
                k = 0.85 if ((x // 8) + (y // 8)) % 2 == 0 else 1.1
                ramp = 0.9 + 0.2 * ((x + y) % 32) / 31.0
                row += [min(255, int(c * k * ramp)) for c in base]
            elif "_nor_" in name:
                row += [128, 128, 255]
            elif "_arm_" in name:
                row += [255, 170, 0]
            else:
                row += [170, 170, 170]
        rows.append(row)
    return png(SIZE, SIZE, rows)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = Path(sys.argv[1])
    lock = json.load(open(LOCK))
    n = 0
    digest = hashlib.sha256()
    for f in sorted(lock["files"], key=lambda f: f["path"]):
        p = f["path"]
        if not p.endswith(".jpg"):
            continue
        data = image_for(p)
        dest = out / p
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        digest.update(p.encode() + data)
        n += 1
    print("fixtures: %d textures in %s (set digest %s)" % (n, out, digest.hexdigest()[:16]))
    return 0 if n > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
