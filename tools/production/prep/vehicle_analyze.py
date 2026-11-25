#!/usr/bin/env python3
"""Identifies a vehicle model's parts and measures it, so that its physics can be configured to match: wheels (centers, radius, width, which axle and side), doors, hood, trunk, steering
wheel, lights; body bounds; wheelbase and track; and a suggested `VehicleConfig` geometry for saige/vehicle.hpp in engine space (+X forward, +Y left, +Z up).

  vehicle_analyze.py CAR.glb [--out manifest.json]

Method: node names (wheel/tire/rim/door/hood/steer ...) group the nodes; each group's bounds come from its meshes' accessor min/max through the node hierarchy. A group is a wheel when its
bounds are nearly round in the plane perpendicular to the axle (the thinnest axis). Works for models authored Y-up with the front along +Z (the glTF convention); the car's length is found
from the wheel layout instead of being assumed.
"""
import json, re, sys
from pathlib import Path
import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gltf_nodes import read_gltf_json, world_nodes, node_world_bounds, descendants, to_engine  # noqa: E402

PATTERNS = {
    "wheel": re.compile(r"wheel|tire|tyre|rim\b", re.I),
    "door": re.compile(r"door", re.I), "hood": re.compile(r"hood|bonnet", re.I), "trunk": re.compile(r"trunk|boot|tailgate|rearhatch", re.I),
    "steering": re.compile(r"steering", re.I), "headlight": re.compile(r"headlight|headlamp", re.I), "taillight": re.compile(r"taillight|brakelight|rearlight", re.I),
    "mirror": re.compile(r"mirror", re.I), "glass": re.compile(r"window|windshield|glass", re.I),
}


def group_bounds(js, worlds, roots):
    lo, hi = np.full(3, 1e30), np.full(3, -1e30)
    for r in roots:
        for d in descendants(js, r):
            b = node_world_bounds(js, worlds, d)
            if b:
                lo, hi = np.minimum(lo, b[0]), np.maximum(hi, b[1])
    return (lo, hi) if lo[0] <= hi[0] else None


def main():
    path = sys.argv[1]
    out = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else None
    js, _ = read_gltf_json(path)
    worlds, parent = world_nodes(js)
    nodes = js["nodes"]
    names = [n.get("name", "") for n in nodes]
    # Top-level wheel groups: nodes whose own name says wheel and whose parent's name does not (so rim/brake children do not become wheels of their own).
    wheel_roots = [i for i, n in enumerate(names) if re.match(r"^wheel", n, re.I) and not re.match(r"^wheel", names[parent[i]], re.I) if i not in parent or True]
    wheel_roots = [i for i in wheel_roots if i not in parent or not re.match(r"^wheel", names[parent[i]] or "", re.I)]
    wheels = []
    for r in wheel_roots:
        b = group_bounds(js, worlds, [r])
        if b is None:
            continue
        lo, hi = b
        ext = hi - lo
        axle = int(np.argmin(ext))  # the thin axis of a wheel
        others = [a for a in range(3) if a != axle]
        diameter = float(max(ext[others]))
        wheels.append({"name": names[r], "center_gltf": ((lo + hi) / 2).tolist(), "axle_axis_gltf": "xyz"[axle], "diameter": diameter, "radius": diameter / 2, "width": float(ext[axle])})
    result = {"file": Path(path).name, "wheels": [], "parts": {}}
    if len(wheels) >= 4:
        centers = np.array([w["center_gltf"] for w in wheels])
        spread = centers.max(axis=0) - centers.min(axis=0)
        horiz = [a for a in range(3) if a != 1]  # glTF up is Y
        length_axis = max(horiz, key=lambda a: spread[a])
        width_axis = [a for a in horiz if a != length_axis][0]
        # engine axes: length -> +X (front is the larger coordinate along the length axis if the hood is there; decided by the hood or the steered wheels' name), width -> +Y (left)
        front_sign = 1.0
        hood = [i for i, n in enumerate(names) if PATTERNS["hood"].search(n)]
        body_lo, body_hi = group_bounds(js, worlds, [i for i, n in enumerate(nodes) if i not in parent]) or (None, None)
        if hood:
            hb = group_bounds(js, worlds, hood[:1])
            hc = (hb[0] + hb[1]) / 2
            front_sign = 1.0 if hc[length_axis] > (body_lo[length_axis] + body_hi[length_axis]) / 2 else -1.0
        for w in wheels:
            c = np.array(w["center_gltf"])
            e = [front_sign * c[length_axis], -(1.0 if width_axis == 0 else 1.0) * c[width_axis], c[1]]
            # sign of the width axis: engine +Y is left. glTF +X is left when +Z is the front (right-handed, Y up); for a front along -Z or a different length axis the sign is derived from handedness.
            cross = np.zeros(3)
            fwd = np.zeros(3); fwd[length_axis] = front_sign
            up = np.array([0, 1.0, 0])
            left = np.cross(up, fwd)  # left = up x forward
            e_y = float(np.dot(c, left))
            result["wheels"].append({"name": w["name"], "center_engine": [round(float(front_sign * c[length_axis]), 4), round(e_y, 4), round(float(c[1]), 4)], "radius_group_bounds": round(w["radius"], 4), "radius_if_resting_on_y0": round(float(c[1]), 4), "width": round(w["width"], 4)})
        ew = np.array([w["center_engine"] for w in result["wheels"]])
        front = ew[ew[:, 0] > ew[:, 0].mean()]
        rear = ew[ew[:, 0] <= ew[:, 0].mean()]
        result["wheelbase"] = round(float(front[:, 0].mean() - rear[:, 0].mean()), 4)
        result["track_front"] = round(float(front[:, 1].max() - front[:, 1].min()), 4)
        result["track_rear"] = round(float(rear[:, 1].max() - rear[:, 1].min()), 4)
        # The group's bounds include brake hardware and anything parented under the wheel node, so they overstate the tire. A wheel standing on the model's ground plane (y = 0) has its
        # center at one radius above it: report that estimate as the radius (check it against the tire mesh before trusting it on a model whose origin is not on the ground).
        result["wheel_radius"] = round(float(np.mean([w["radius_if_resting_on_y0"] for w in result["wheels"]])), 4)
        fwd = np.zeros(3); fwd[length_axis] = front_sign
        left = np.cross(np.array([0, 1.0, 0]), fwd)
        body_nodes = [i for i, n in enumerate(names) if i not in parent and not PATTERNS["wheel"].search(n)]
        bb = group_bounds(js, worlds, body_nodes)
        if bb:
            lo, hi = bb
            corners = np.array([[x, y, z] for x in (lo[0], hi[0]) for y in (lo[1], hi[1]) for z in (lo[2], hi[2])])
            ex = np.stack([corners @ fwd, corners @ left, corners[:, 1]], axis=1)
            result["body_bounds_engine"] = {"min": ex.min(axis=0).round(4).tolist(), "max": ex.max(axis=0).round(4).tolist()}
            bmin, bmax = ex.min(axis=0), ex.max(axis=0)
            center = (bmin + bmax) / 2
            # suggested VehicleConfig geometry: the chassis box centered on the body in x/y; wheel mounts relative to it
            result["suggested_vehicle_config"] = {
                "half_extents": [round(float((bmax[0] - bmin[0]) / 2), 3), round(float((bmax[1] - bmin[1]) / 2), 3), round(float((bmax[2] - bmin[2]) / 2), 3)],
                "wheel_radius": result["wheel_radius"],
                "box_center_in_model": [round(float(center[0]), 4), round(float(center[1]), 4), None],
                "wheel_mounts_relative_to_box_center_xy": [[round(float(w["center_engine"][0] - center[0]), 4), round(float(w["center_engine"][1] - center[1]), 4)] for w in result["wheels"]],
                "note": "z: choose suspension travel and static compression, then box_center_z = wheel_center_z - (mount_z - travel + compression)"}
    for key in ("door", "hood", "trunk", "steering", "headlight", "taillight", "mirror"):
        roots = [i for i, n in enumerate(names) if PATTERNS[key].search(n) and (i not in parent or not PATTERNS[key].search(names[parent[i]]))]
        if roots:
            result["parts"][key] = [names[i] for i in roots]
    doors = result["parts"].get("door", [])
    result["doors"] = {"left": [d for d in doors if re.search(r"(^|[^a-z])l(eft)?([^a-z]|$|color|handle)|doorl|_l\b|left", d, re.I)], "right": [d for d in doors if re.search(r"doorr|right|_r\b", d, re.I)]}
    result["identified"] = {"wheels": len(result["wheels"]), "four_wheels": len(result["wheels"]) == 4, "doors": len(doors) > 0, "hood": "hood" in result["parts"]}
    text = json.dumps(result, indent=1)
    if out:
        Path(out).write_text(text)
    print(text)


main()
