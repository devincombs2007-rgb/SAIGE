#!/usr/bin/env python3
"""Collision geometry for a model, from its render mesh: a box, a convex hull, or a simplified triangle mesh, written as JSON in SAIGE's StaticCollision terms (boxes and triangles in meters,
engine axes +X forward, +Y left, +Z up).

  collision_hull.py MODEL.glb OUT.json [--mode box|hull|mesh] [--max-triangles 300]

`box`: the axis-aligned bounds (fast, for crates and buildings). `hull`: the convex hull, decimated to at most --max-triangles (for rocks, barrels, barriers). `mesh`: the model's triangles
quadric-simplified to at most --max-triangles (for terrain-like or concave props; static only). Nothing here replaces looking at the result: the JSON records the volume ratio of the hull to
the bounds so a hull that is mostly empty space is visible.
"""
import json, sys
from pathlib import Path
import numpy as np
import trimesh


def main():
    src, out = sys.argv[1], sys.argv[2]
    mode = sys.argv[sys.argv.index("--mode") + 1] if "--mode" in sys.argv else "hull"
    max_tris = int(sys.argv[sys.argv.index("--max-triangles") + 1]) if "--max-triangles" in sys.argv else 300
    scene = trimesh.load(src, force="scene")
    mesh = scene.dump(concatenate=True)
    v = mesh.vertices
    # glTF (x, y, z) -> engine (z, x, y), the importer's mapping; winding is preserved by a proper rotation
    ev = np.stack([v[:, 2], v[:, 0], v[:, 1]], axis=1)
    mesh = trimesh.Trimesh(ev, mesh.faces, process=True)
    lo, hi = mesh.bounds
    result = {"source": Path(src).name, "mode": mode, "bounds_min": lo.round(4).tolist(), "bounds_max": hi.round(4).tolist(), "boxes": [], "triangles": []}
    if mode == "box":
        result["boxes"].append({"min": lo.round(4).tolist(), "max": hi.round(4).tolist()})
    else:
        geom = mesh.convex_hull if mode == "hull" else mesh
        if len(geom.faces) > max_tris:
            import fast_simplification
            vv, ff = fast_simplification.simplify(geom.vertices.astype(np.float32), geom.faces.astype(np.int32), target_count=max_tris)
            geom = trimesh.Trimesh(vv, ff, process=True)
            if mode == "hull":
                geom = geom.convex_hull
        result["triangles"] = [[geom.vertices[i].round(4).tolist() for i in f] for f in geom.faces]
        if mode == "hull":
            box_vol = float(np.prod(hi - lo)) or 1.0
            result["hull_volume_over_bounds_volume"] = round(float(geom.volume) / box_vol, 3)
    result["triangle_count"] = len(result["triangles"])
    Path(out).write_text(json.dumps(result))
    print(json.dumps({k: v for k, v in result.items() if k != "triangles"}))


main()
