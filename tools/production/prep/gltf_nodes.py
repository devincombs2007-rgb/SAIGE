"""Shared glTF helpers: node world matrices and per-node mesh bounds (numpy), engine-space conversion. glTF is Y-up with the front at +Z; SAIGE's importer maps (x, y, z) to (z, x, y)."""
import sys
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from saige_assets.extract import read_gltf_json  # noqa: E402


def local_matrix(node):
    if "matrix" in node:
        return np.array(node["matrix"], dtype=float).reshape(4, 4).T
    t = np.array(node.get("translation", [0, 0, 0]), dtype=float)
    q = np.array(node.get("rotation", [0, 0, 0, 1]), dtype=float)
    s = np.array(node.get("scale", [1, 1, 1]), dtype=float)
    x, y, z, w = q / (np.linalg.norm(q) or 1)
    r = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)], [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)], [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = r * s
    m[:3, 3] = t
    return m


def world_nodes(js):
    nodes = js.get("nodes", [])
    parent = {}
    for i, n in enumerate(nodes):
        for c in n.get("children", []):
            parent[c] = i
    cache = {}

    def world(i):
        if i not in cache:
            m = local_matrix(nodes[i])
            cache[i] = world(parent[i]) @ m if i in parent else m
        return cache[i]
    return [world(i) for i in range(len(nodes))], parent


def mesh_local_bounds(js, mesh_index):
    acc = js["accessors"]
    lo, hi = np.full(3, 1e30), np.full(3, -1e30)
    for p in js["meshes"][mesh_index]["primitives"]:
        a = acc[p["attributes"]["POSITION"]]
        if a.get("min") and a.get("max"):
            lo, hi = np.minimum(lo, a["min"]), np.maximum(hi, a["max"])
    return lo, hi


def node_world_bounds(js, worlds, node_index):
    n = js["nodes"][node_index]
    if "mesh" not in n:
        return None
    lo, hi = mesh_local_bounds(js, n["mesh"])
    if lo[0] > hi[0]:
        return None
    corners = np.array([[x, y, z, 1.0] for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])])
    w = (worlds[node_index] @ corners.T).T[:, :3]
    return w.min(axis=0), w.max(axis=0)


def descendants(js, i):
    out = [i]
    for c in js["nodes"][i].get("children", []):
        out += descendants(js, c)
    return out


def to_engine(p):
    return np.array([p[2], p[0], p[1]])
