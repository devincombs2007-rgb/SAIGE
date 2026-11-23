"""Builds the Showcase 01 courtyard as glTF 2.0 (plus the character, a decal quad, and scene.json with collision and the route).

Units are meters; authored in engine space (+Z up, +X forward, +Y left), +X is the courtyard's long axis, and converted to glTF axes on write. Everything here is original geometry. The same boxes that are drawn
are written to scene.json as colliders, so the character collides with what the camera sees (round props use their bounding box,
which is noted next to each).
"""
import base64
import json
import math
import os
import struct

import numpy as np

SAMPLER = {"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497}


class Mesh:
    """Triangle soup grouped by material; written as one glTF mesh with one primitive per material."""

    def __init__(self):
        self.prims = {}  # material index -> dict(p, n, uv, idx)

    def _prim(self, mat):
        return self.prims.setdefault(mat, {"p": [], "n": [], "uv": [], "i": []})

    def quad(self, mat, p0, p1, p2, p3, n, uv0, uv1, uv2, uv3):
        pr = self._prim(mat)
        base = len(pr["p"])
        for p, uv in zip((p0, p1, p2, p3), (uv0, uv1, uv2, uv3)):
            pr["p"].append(p)
            pr["n"].append(n)
            pr["uv"].append(uv)
        pr["i"] += [base, base + 1, base + 2, base, base + 2, base + 3]  # CCW seen from outside

    def box(self, mat, lo, hi, scale, top_mat=None, uv_origin=(0.0, 0.0)):
        """Axis-aligned box; texture repeats once per `scale` meters. `top_mat` overrides the material of the +Z face."""
        x0, y0, z0 = lo
        x1, y1, z1 = hi
        ox, oy = uv_origin
        s = 1.0 / scale
        # +Z (top): u = x, v = -y
        self.quad(top_mat if top_mat is not None else mat, (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1), (0, 0, 1),
                  ((x0 + ox) * s, -(y0 + oy) * s), ((x1 + ox) * s, -(y0 + oy) * s), ((x1 + ox) * s, -(y1 + oy) * s), ((x0 + ox) * s, -(y1 + oy) * s))
        # -Z (bottom)
        self.quad(mat, (x0, y1, z0), (x1, y1, z0), (x1, y0, z0), (x0, y0, z0), (0, 0, -1),
                  (x0 * s, -y1 * s), (x1 * s, -y1 * s), (x1 * s, -y0 * s), (x0 * s, -y0 * s))
        # +X: u = y, v = -z
        self.quad(mat, (x1, y0, z0), (x1, y1, z0), (x1, y1, z1), (x1, y0, z1), (1, 0, 0),
                  (y0 * s, -z0 * s), (y1 * s, -z0 * s), (y1 * s, -z1 * s), (y0 * s, -z1 * s))
        # -X
        self.quad(mat, (x0, y1, z0), (x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (-1, 0, 0),
                  (-y1 * s, -z0 * s), (-y0 * s, -z0 * s), (-y0 * s, -z1 * s), (-y1 * s, -z1 * s))
        # +Y: u = -x
        self.quad(mat, (x1, y1, z0), (x0, y1, z0), (x0, y1, z1), (x1, y1, z1), (0, 1, 0),
                  (-x1 * s, -z0 * s), (-x0 * s, -z0 * s), (-x0 * s, -z1 * s), (-x1 * s, -z1 * s))
        # -Y: u = x
        self.quad(mat, (x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1), (0, -1, 0),
                  (x0 * s, -z0 * s), (x1 * s, -z0 * s), (x1 * s, -z1 * s), (x0 * s, -z1 * s))

    def prism(self, mat, cx, cy, z0, z1, radius, sides, scale, cap_mat=None):
        """Vertical n-gon column with smooth side normals and flat caps."""
        pr = self._prim(mat)
        s = 1.0 / scale
        ring = [(cx + radius * math.cos(2 * math.pi * k / sides), cy + radius * math.sin(2 * math.pi * k / sides)) for k in range(sides + 1)]
        for k in range(sides):
            (xa, ya), (xb, yb) = ring[k], ring[k + 1]
            na = (math.cos(2 * math.pi * k / sides), math.sin(2 * math.pi * k / sides), 0.0)
            nb = (math.cos(2 * math.pi * (k + 1) / sides), math.sin(2 * math.pi * (k + 1) / sides), 0.0)
            u0, u1 = k / sides * 2 * math.pi * radius * s, (k + 1) / sides * 2 * math.pi * radius * s
            base = len(pr["p"])
            for p, n, uv in (((xa, ya, z0), na, (u0, -z0 * s)), ((xb, yb, z0), nb, (u1, -z0 * s)),
                             ((xb, yb, z1), nb, (u1, -z1 * s)), ((xa, ya, z1), na, (u0, -z1 * s))):
                pr["p"].append(p); pr["n"].append(n); pr["uv"].append(uv)
            pr["i"] += [base, base + 1, base + 2, base, base + 2, base + 3]
        cap = self._prim(cap_mat if cap_mat is not None else mat)
        for z, nz in ((z1, 1.0), (z0, -1.0)):
            c = len(cap["p"])
            cap["p"].append((cx, cy, z)); cap["n"].append((0, 0, nz)); cap["uv"].append((cx * s, -cy * s))
            for k in range(sides + 1):
                x, y = ring[k]
                cap["p"].append((x, y, z)); cap["n"].append((0, 0, nz)); cap["uv"].append((x * s, -y * s))
            for k in range(sides):
                a, b = c + 1 + k, c + 2 + k
                cap["i"] += [c, a, b] if nz > 0 else [c, b, a]

    def sphere(self, mat, center, radius, segments=48, rings=24):
        pr = self._prim(mat)
        cx, cy, cz = center
        base = len(pr["p"])
        for r in range(rings + 1):
            phi = math.pi * r / rings
            for sgm in range(segments + 1):
                th = 2 * math.pi * sgm / segments
                n = (math.sin(phi) * math.cos(th), math.sin(phi) * math.sin(th), math.cos(phi))
                pr["p"].append((cx + radius * n[0], cy + radius * n[1], cz + radius * n[2]))
                pr["n"].append(n)
                pr["uv"].append((sgm / segments, r / rings))
        for r in range(rings):
            for sgm in range(segments):
                a = base + r * (segments + 1) + sgm
                b = a + segments + 1
                pr["i"] += [a, a + 1, b + 1, a, b + 1, b]

    def capsule(self, mat, radius, z_bottom, z_top, segments=32, rings=12):
        """Capsule with its lowest point at z_bottom and highest at z_top (a hemisphere cap at each end)."""
        pr = self._prim(mat)
        base = len(pr["p"])
        zb, zt = z_bottom + radius, z_top - radius
        profile = []  # (radial, z, normal_radial, normal_z)
        for r in range(rings + 1):  # bottom hemisphere, pole to equator
            a = -math.pi / 2 + (math.pi / 2) * r / rings
            profile.append((radius * math.cos(a), zb + radius * math.sin(a), math.cos(a), math.sin(a)))
        for r in range(rings + 1):  # top hemisphere, equator to pole
            a = (math.pi / 2) * r / rings
            profile.append((radius * math.cos(a), zt + radius * math.sin(a), math.cos(a), math.sin(a)))
        for j, (rad, z, nr, nz) in enumerate(profile):
            for sgm in range(segments + 1):
                th = 2 * math.pi * sgm / segments
                pr["p"].append((rad * math.cos(th), rad * math.sin(th), z))
                pr["n"].append((nr * math.cos(th), nr * math.sin(th), nz))
                pr["uv"].append((sgm / segments, j / (len(profile) - 1)))
        for j in range(len(profile) - 1):
            for sgm in range(segments):
                a = base + j * (segments + 1) + sgm
                b = a + segments + 1
                pr["i"] += [a, a + 1, b + 1, a, b + 1, b]


def gltf_document(mesh, materials, textures, images, extra_extensions=False):
    buffer = bytearray()
    views, accessors, primitives = [], [], []

    def add(data, target, comp, count, kind, minmax=None):
        while len(buffer) % 4:
            buffer.append(0)
        views.append({"buffer": 0, "byteOffset": len(buffer), "byteLength": len(data), "target": target})
        buffer.extend(data)
        acc = {"bufferView": len(views) - 1, "componentType": comp, "count": count, "type": kind}
        if minmax:
            acc["min"], acc["max"] = minmax
        accessors.append(acc)
        return len(accessors) - 1

    for mat, pr in sorted(mesh.prims.items()):
        # Geometry is authored in engine space (+Z up, +X forward). glTF is +Y up with front +Z, and the importer maps
        # (x, y, z) -> (z, x, y), so the file stores (y, z, x) for an engine-space point (x, y, z).
        p = np.asarray(pr["p"], dtype=np.float32)[:, [1, 2, 0]].copy()
        n = np.asarray(pr["n"], dtype=np.float32)[:, [1, 2, 0]].copy()
        uv = np.asarray(pr["uv"], dtype=np.float32)
        idx = np.asarray(pr["i"], dtype=np.uint32)
        ap = add(p.tobytes(), 34962, 5126, len(p), "VEC3", ([float(v) for v in p.min(0)], [float(v) for v in p.max(0)]))
        an = add(n.tobytes(), 34962, 5126, len(n), "VEC3")
        au = add(uv.tobytes(), 34962, 5126, len(uv), "VEC2")
        ai = add(idx.tobytes(), 34963, 5125, len(idx), "SCALAR")
        primitives.append({"attributes": {"POSITION": ap, "NORMAL": an, "TEXCOORD_0": au}, "indices": ai, "material": mat})
    doc = {
        "asset": {"version": "2.0", "generator": "SAIGE Showcase 01 scene.py (original geometry)"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}], "meshes": [{"primitives": primitives}],
        "materials": materials, "accessors": accessors, "bufferViews": views,
        "buffers": [{"byteLength": len(buffer), "uri": "data:application/octet-stream;base64," + base64.b64encode(bytes(buffer)).decode()}],
    }
    if textures:
        doc["samplers"] = [SAMPLER]
        doc["textures"] = textures
        doc["images"] = images
    uses_emissive = any("extensions" in m for m in materials)
    if uses_emissive:
        doc["extensionsUsed"] = ["KHR_materials_emissive_strength"]
    return doc


class Materials:
    def __init__(self):
        self.textures, self.images, self.materials, self.index = [], [], [], {}

    def image(self, uri):
        self.images.append({"uri": uri})
        self.textures.append({"sampler": 0, "source": len(self.images) - 1})
        return len(self.textures) - 1

    def textured(self, name, set_name, normal_scale=1.0, base_tint=(1, 1, 1)):
        a, n, o = (self.image(f"{set_name}_{k}.png") for k in ("albedo", "normal", "orm"))
        self.materials.append({"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*base_tint, 1.0], "baseColorTexture": {"index": a},
                                                                        "metallicFactor": 1.0, "roughnessFactor": 1.0, "metallicRoughnessTexture": {"index": o}},
                               "normalTexture": {"index": n, "scale": normal_scale}, "occlusionTexture": {"index": o, "strength": 1.0}})
        self.index[name] = len(self.materials) - 1

    def plain(self, name, color, metallic, roughness, alpha=None, emissive=None, emissive_strength=None, double_sided=False):
        m = {"name": name, "pbrMetallicRoughness": {"baseColorFactor": [*color, 1.0 if alpha is None else alpha],
                                                      "metallicFactor": metallic, "roughnessFactor": roughness}}
        if alpha is not None:
            m["alphaMode"] = "BLEND"
        if emissive is not None:
            m["emissiveFactor"] = list(emissive)
            if emissive_strength is not None:
                m["extensions"] = {"KHR_materials_emissive_strength": {"emissiveStrength": emissive_strength}}
        if double_sided:
            m["doubleSided"] = True
        self.materials.append(m)
        self.index[name] = len(self.materials) - 1

    def __getitem__(self, name):
        return self.index[name]


def build_courtyard():
    M = Materials()
    M.textured("cobble", "cobble")
    M.textured("brick", "brick")
    M.textured("wood", "wood", 1.2)
    M.textured("metal", "metal", 1.0)
    M.textured("marble", "marble", 0.8, base_tint=(0.82, 0.82, 0.84))
    M.plain("glass", (0.55, 0.75, 0.90), 0.0, 0.05, alpha=0.24, double_sided=True)
    M.plain("lantern", (0.9, 0.6, 0.25), 0.0, 0.4, emissive=(1.0, 0.55, 0.18), emissive_strength=7.0)
    spheres = [("ball_clay", (0.62, 0.18, 0.12), 0.0, 0.95), ("ball_plastic", (0.12, 0.30, 0.65), 0.0, 0.45),
               ("ball_glossy", (0.10, 0.50, 0.25), 0.0, 0.10), ("ball_gold", (1.00, 0.77, 0.34), 1.0, 0.45), ("ball_chrome", (0.90, 0.90, 0.92), 1.0, 0.12)]
    for name, color, metallic, rough in spheres:
        M.plain(name, color, metallic, rough)

    mesh = Mesh()
    colliders = []
    decals = []

    def solid(name, mat, lo, hi, scale, top_mat=None, collide=True, uv_origin=(0.0, 0.0)):
        mesh.box(M[mat], lo, hi, scale, None if top_mat is None else M[top_mat], uv_origin)
        if collide:
            colliders.append({"name": name, "min": list(lo), "max": list(hi)})

    # ---- Ground: a cobbled courtyard, one repeat per 2 m. The top is at z = 0, the collision floor is a slab below it.
    solid("ground", "cobble", (-22, -16, -1.0), (26, 16, 0.0), 2.0)

    # ---- The dais: three marble steps (0.2 m rise each, 0.55 m tread) up to a 0.6 m platform.
    for k in range(3):
        solid(f"step{k}", "marble", (5.5 - 0.55 * (3 - k), -2.2, 0.0), (6.0, 2.2, 0.2 * (k + 1)), 1.6)
    solid("dais", "marble", (6.0, -6.0, 0.0), (17.0, 6.0, 0.6), 1.6)
    solid("dais_trim", "marble", (6.0, -6.0, 0.6), (17.0, 6.0, 0.62), 1.6, collide=False)

    # ---- Walls: brick, 4 m, with a gate in the west wall. Marble coping on top. Buttresses every 6 m.
    solid("wall_east", "brick", (18.0, -13.0, 0.0), (19.2, 13.0, 4.6), 1.6)
    solid("wall_north", "brick", (-19.0, 12.0, 0.0), (19.2, 13.0, 4.0), 1.6)
    solid("wall_south", "brick", (-19.0, -13.0, 0.0), (19.2, -12.0, 4.0), 1.6)
    solid("wall_west_a", "brick", (-19.0, -13.0, 0.0), (-18.0, -3.5, 4.0), 1.6)
    solid("wall_west_b", "brick", (-19.0, 3.5, 0.0), (-18.0, 13.0, 4.0), 1.6)
    for name, lo, hi in (("coping_n", (-19.1, 11.9, 4.0), (19.3, 13.1, 4.18)), ("coping_s", (-19.1, -13.1, 4.0), (19.3, -11.9, 4.18)),
                         ("coping_wa", (-19.1, -13.1, 4.0), (-17.9, -3.4, 4.18)), ("coping_wb", (-19.1, 3.4, 4.0), (-17.9, 13.1, 4.18)),
                         ("coping_e", (17.9, -13.1, 4.6), (19.3, 13.1, 4.78))):
        solid(name, "marble", lo, hi, 1.6, collide=False)
    for x in (-12.0, -6.0, 0.0, 6.0, 12.0):
        for side, y in (("n", 11.2), ("s", -12.0)):
            solid(f"buttress_{side}{int(x)}", "brick", (x - 0.45, y, 0.0), (x + 0.45, y + 0.8, 4.3), 1.6)

    # ---- Colonnade: two rows of marble columns carrying lintels. Columns collide as their bounding squares.
    for y in (-7.0, 7.0):
        for x in (-12.0, -8.0, -4.0, 0.0):
            mesh.prism(M["marble"], x, y, 0.0, 3.5, 0.42, 14, 1.6, cap_mat=M["marble"])
            colliders.append({"name": f"column_{int(x)}_{int(y)}", "min": [x - 0.42, y - 0.42, 0.0], "max": [x + 0.42, y + 0.42, 3.5]})
            solid(f"plinth_{int(x)}_{int(y)}", "marble", (x - 0.6, y - 0.6, 0.0), (x + 0.6, y + 0.6, 0.25), 1.6)
            decals.append({"x": x, "y": y, "width": 2.6, "depth": 2.6, "z": 0.02, "strength": 0.8})
        solid(f"lintel_{int(y)}", "marble", (-12.7, y - 0.55, 3.5), (0.7, y + 0.55, 3.95), 1.6, collide=False)
    for x in (-12.0, 0.0):
        for y in (-7.0, 7.0):
            mesh.box(M["lantern"], (x - 0.14, y - 0.14, 3.95), (x + 0.14, y + 0.14, 4.3), 1.0)

    # ---- Crates: a jumping trail (0.5 m, then 1.0 m) and a loose pair. 0.35 m is the step-up limit, so these need a jump.
    for name, x, y, h in (("crate_a", -6.0, 3.6, 0.5), ("crate_b", -4.7, 3.6, 1.0), ("crate_c", -6.0, -3.6, 0.6),
                          ("crate_d", -5.3, -4.6, 0.6), ("crate_e", -5.5, -4.1, 1.2)):
        half = 0.5 if h >= 0.9 else 0.45
        if name == "crate_e":
            lo, hi = (-5.9, -4.5, 0.6), (-5.1, -3.7, 1.2)  # stacked on c/d
        else:
            lo, hi = (x - half, y - half, 0.0), (x + half, y + half, h)
        solid(name, "wood", lo, hi, 1.0)
        decals.append({"x": (lo[0] + hi[0]) / 2, "y": (lo[1] + hi[1]) / 2, "width": 2 * half + 1.4, "depth": 2 * half + 1.4, "z": 0.02, "strength": 0.9})

    # ---- Rusty barrels (bounding squares collide).
    for x, y in ((-10.5, -4.5), (-9.9, -3.7), (-10.8, 4.8)):
        mesh.prism(M["metal"], x, y, 0.0, 1.0, 0.42, 16, 1.0, cap_mat=M["metal"])
        colliders.append({"name": f"barrel_{x}_{y}", "min": [x - 0.42, y - 0.42, 0.0], "max": [x + 0.42, y + 0.42, 1.0]})
        decals.append({"x": x, "y": y, "width": 2.0, "depth": 2.0, "z": 0.02, "strength": 0.85})

    # ---- Material row: five spheres on marble pedestals on the dais, varying roughness and metalness (back row, y from -4 to 4).
    for k, (name, *_rest) in enumerate(spheres):
        y = -3.6 + 1.8 * k
        solid(f"pedestal{k}", "marble", (15.3, y - 0.3, 0.62), (15.9, y + 0.3, 1.5), 1.6)
        mesh.sphere(M[name], (15.6, y, 1.5 + 0.34), 0.34)
        decals.append({"x": 15.6, "y": y, "width": 0.62, "depth": 0.62, "z": 1.505, "strength": 0.8})

    # ---- A glass screen in front of the east wall (alpha blended, sorted by the renderer), and a brass-colored sconce strip.
    solid("glass_screen", "glass", (16.4, -2.6, 0.62), (16.45, 2.6, 2.9), 1.0)
    solid("glass_frame_l", "metal", (16.35, -2.7, 0.62), (16.5, -2.6, 2.95), 1.0)
    solid("glass_frame_r", "metal", (16.35, 2.6, 0.62), (16.5, 2.7, 2.95), 1.0)
    solid("glass_frame_t", "metal", (16.35, -2.7, 2.9), (16.5, 2.7, 2.95), 1.0)

    doc = gltf_document(mesh, M.materials, M.textures, M.images)

    route = [
        {"x": -15.0, "y": 0.0}, {"x": -12.0, "y": 0.0}, {"x": -7.0, "y": 1.6},
        {"x": -6.3, "y": 3.6, "jump_distance": 1.0},                        # jump onto the 0.5 m crate (above the 0.35 m step limit)
        {"x": -4.8, "y": 3.6, "jump_distance": 0.9},                        # and on to the 1.0 m crate
        {"x": -2.0, "y": 1.0}, {"x": 1.0, "y": 0.0, "sprint": True},
        {"x": 4.8, "y": 0.0}, {"x": 7.0, "y": 0.0},                         # up the three 0.2 m marble steps onto the dais
        {"x": 12.0, "y": 2.5}, {"x": 13.6, "y": -1.0},
        {"x": 13.0, "y": 5.4}, {"x": 14.4, "y": 5.4},
        {"x": 14.4, "y": -5.4}, {"x": 9.0, "y": -5.4, "sprint": True}, {"x": 5.0, "y": -5.4},   # off the dais edge (a 0.6 m drop)
        {"x": 3.0, "y": -5.2}, {"x": -2.0, "y": -9.0, "sprint": True}, {"x": -9.0, "y": -9.0},
        {"x": -13.0, "y": -5.0},
        {"x": -8.0, "y": 0.5}, {"x": -2.0, "y": 0.5}, {"x": -2.0, "y": 8.0}, {"x": 1.5, "y": 8.0},
        {"x": 4.5, "y": 14.5, "max_seconds": 2.6, "timed": True},       # runs into the north wall and slides along it
        {"x": 4.5, "y": 8.0}, {"x": 3.0, "y": 3.0}, {"x": 4.8, "y": 0.0}, {"x": 7.0, "y": 0.0}, {"x": 13.4, "y": 0.0},
    ]
    return doc, colliders, decals, route


def build_character():
    M = Materials()
    M.plain("suit", (0.80, 0.27, 0.06), 0.0, 0.55)
    M.plain("visor", (0.02, 0.03, 0.05), 0.0, 0.12)
    M.plain("pack", (0.16, 0.17, 0.18), 0.0, 0.8)
    mesh = Mesh()
    mesh.capsule(M["suit"], 0.3, 0.0, 1.7)
    mesh.box(M["visor"], (0.17, -0.21, 1.20), (0.34, 0.21, 1.42), 1.0)
    mesh.box(M["pack"], (-0.36, -0.17, 0.85), (-0.24, 0.17, 1.30), 1.0)
    return gltf_document(mesh, M.materials, [], [])


def build_decal():
    M = Materials()
    M.textures.append({"sampler": 0, "source": 0})
    M.images.append({"uri": "blob_shadow.png"})
    M.materials.append({"name": "blob", "pbrMetallicRoughness": {"baseColorFactor": [1, 1, 1, 1], "baseColorTexture": {"index": 0},
                                                                   "metallicFactor": 0.0, "roughnessFactor": 1.0}, "alphaMode": "BLEND", "doubleSided": True})
    mesh = Mesh()
    mesh.quad(0, (-0.5, -0.5, 0), (0.5, -0.5, 0), (0.5, 0.5, 0), (-0.5, 0.5, 0), (0, 0, 1), (0, 1), (1, 1), (1, 0), (0, 0))
    return gltf_document(mesh, M.materials, M.textures, M.images)


def write_all(out_dir):
    os.makedirs(out_dir, exist_ok=True)
    courtyard, colliders, decals, route = build_courtyard()
    for name, doc in (("courtyard.gltf", courtyard), ("character.gltf", build_character()), ("blob.gltf", build_decal())):
        with open(os.path.join(out_dir, name), "w") as f:
            json.dump(doc, f, separators=(",", ":"))
    with open(os.path.join(out_dir, "scene.json"), "w") as f:
        json.dump({"schema_version": 1, "character": {"radius": 0.3, "height": 1.7, "spawn": [-16.5, 0.0, 0.0]}, "colliders": colliders,
                   "decals": decals, "route": route}, f, indent=1)
    return len(colliders), len(decals)
