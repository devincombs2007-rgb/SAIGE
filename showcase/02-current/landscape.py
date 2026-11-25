#!/usr/bin/env python3
"""Build the courtyard's distant landscape with a CC0 Poly Haven texture."""
import json
import math
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "01-courtyard"))
from scene import Mesh, gltf_document

def height(x, y):
    radius = math.hypot(x, y)
    blend = min(1.0, max(0.0, (radius - 32.0) / 42.0))
    outer = min(1.0, max(0.0, (radius - 180.0) / 200.0))
    return -0.16 + outer * 24.0 + blend * (1.8 + 3.0 * math.sin(x / 33.0 + math.cos(y / 72.0))
                            + 3.0 * math.sin(y / 39.0) + 0.9 * math.sin((x - y) / 11.0))

mesh = Mesh()
prim = mesh._prim(0)
size = 73
for iy in range(size):
    for ix in range(size):
        x = 12.0 * (ix - size // 2)
        y = 12.0 * (iy - size // 2)
        nx = (height(x - 0.1, y) - height(x + 0.1, y)) * 5.0
        ny = (height(x, y - 0.1) - height(x, y + 0.1)) * 5.0
        length = math.sqrt(nx * nx + ny * ny + 1)
        prim["p"].append((x, y, height(x, y)))
        prim["n"].append((nx / length, ny / length, 1 / length))
        prim["uv"].append((x / 9.0, -y / 9.0))
        if ix + 1 < size and iy + 1 < size:
            a = iy * size + ix
            prim["i"].extend((a, a + 1, a + size + 1, a, a + size + 1, a + size))

material = {"name": "Aerial grass and rock", "pbrMetallicRoughness": {
    "baseColorTexture": {"index": 0}, "baseColorFactor": [0.64, 0.60, 0.50, 1],
    "metallicFactor": 0, "roughnessFactor": 0.98}}
path = Path(sys.argv[1])
path.write_text(json.dumps(gltf_document(mesh, [material], [{"sampler": 0, "source": 0}],
                                        [{"uri": "textures/landscape.jpg"}])))
