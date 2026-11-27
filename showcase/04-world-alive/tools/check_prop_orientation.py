#!/usr/bin/env python3
"""Check the front axis of the chair model against the convention the world generator and the SEAT_FACING validator rule assume.

The renderer places a prop model at rot_z(yaw); the importer converts every glTF asset (+Y up, front +Z) to engine space with (x, y, z)_engine = (z, x, y)_glTF
(docs/rendering/gltf-subset.md). So a chair whose seat back is on the glTF -Z side has its seat back on engine -X and its front on +X: yaw 0 faces +X, and a chair at world yaw
`a` faces (cos a, sin a). This reads the real mesh (Poly Haven plastic_monobloc_chair_01, hash-checked by assets.lock.json) and verifies exactly that, so a model swap that changes the
front axis fails here instead of turning every seat sideways again.

  check_prop_orientation.py [--assets DIR]     exit 0 ok, 1 the front axis is not +X, 77 the asset is not restored (a failure with SAIGE_REQUIRE_VISUAL=1)
"""
import argparse, json, os, struct, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--assets", default="/tmp/saige-cache")
    a = ap.parse_args()
    base = os.path.join(a.assets, "polyhaven", "plastic_monobloc_chair_01", "1k")
    gltf, binf = os.path.join(base, "plastic_monobloc_chair_01_1k.gltf"), os.path.join(base, "plastic_monobloc_chair_01.bin")
    if not (os.path.isfile(gltf) and os.path.isfile(binf)):
        print("the chair asset is not restored in", a.assets)
        return 1 if os.environ.get("SAIGE_REQUIRE_VISUAL") == "1" else 77
    g = json.load(open(gltf)); buf = open(binf, "rb").read()
    prim = g["meshes"][0]["primitives"][0]
    acc = g["accessors"][prim["attributes"]["POSITION"]]; bv = g["bufferViews"][acc["bufferView"]]
    off = bv.get("byteOffset", 0) + acc.get("byteOffset", 0); stride = bv.get("byteStride", 12)
    pts = [struct.unpack_from("<3f", buf, off + stride * i) for i in range(acc["count"])]
    # glTF (x, y, z) -> engine (z, x, y)
    eng = [(z, x, y) for (x, y, z) in pts]
    height = max(p[2] for p in eng) - min(p[2] for p in eng)
    top = [p for p in eng if p[2] > min(q[2] for q in eng) + 0.7 * height]   # the seat back is the upper part
    if len(top) < 100:
        print("unexpected mesh: only", len(top), "vertices in the upper 30 percent"); return 1
    cx = sum(p[0] for p in top) / len(top); cy = sum(p[1] for p in top) / len(top)
    print("chair height %.3f m; upper-part centroid in engine axes: x %.3f y %.3f (the seat back; the front is the opposite side)" % (height, cx, cy))
    if cx < -0.05 and abs(cy) < 0.05:
        print("OK: the seat back is on -X, so the chair's front is +X at yaw 0, as the generator and the SEAT_FACING rule assume")
        return 0
    print("FAIL: the seat back is not on -X; the chair front axis changed. Update add_chair_facing and the validator's front convention together.")
    return 1

sys.exit(main())
