#!/usr/bin/env python3
"""Fetches the licensed assets Showcase 03 uses, verifies their checksums and writes assets.json (source URL, licence, authors, per-file md5).

Poly Haven assets are CC0-1.0 (https://polyhaven.com/license). The Khronos glTF sample assets carry their own licences, read from each model's LICENSE.md:
CarConcept and CesiumMan are CC-BY-4.0 (attribution required, logos excluded).

  fetch_assets.py OUT_DIR            download (skipping files that already match) and write OUT_DIR/assets.json
  fetch_assets.py OUT_DIR --verify   check what is on disk against the committed assets.json next to this script
"""
import hashlib, json, os, sys, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
API = "https://api.polyhaven.com"
KHRONOS = "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models"

TEXTURES = {  # name: role
    "asphalt_04": "road",
    "concrete_pavement": "sidewalk",
    "leafy_grass": "ground",
    "gravel_floor": "shoulder",
    "brick_wall_001": "wall_brick",
    "white_stucco": "wall_plaster",
    "clay_roof_tiles_03": "roof_tiles",
    "corrugated_iron": "roof_metal",
    "concrete_floor": "apron",
    "bark_brown_01": "bark",
    "forest_leaves_02": "foliage",
    "aerial_grass_rock": "far_hills",
}
MODELS = ["concrete_road_barrier_02", "street_lamp_01", "utility_box_01", "rock_09", "rock_07", "shrub_sorrel_01", "shrub_03", "barrel_03", "wooden_barrels_01", "painted_wooden_bench"]
KHRONOS_MODELS = {"CarConcept": "CC-BY-4.0", "CesiumMan": "CC-BY-4.0"}


def fetch(url, dest):
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    req = urllib.request.Request(url, headers={"User-Agent": "saige-showcase-fetch"})
    with urllib.request.urlopen(req, timeout=120) as r, open(dest, "wb") as f:
        f.write(r.read())


def md5_of(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def get_json(url):
    req = urllib.request.Request(url, headers={"User-Agent": "saige-showcase-fetch"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.loads(r.read())


def ensure(url, dest, md5=None, record=None):
    if not (os.path.exists(dest) and (md5 is None or md5_of(dest) == md5)):
        fetch(url, dest)
        if md5 is not None and md5_of(dest) != md5:
            raise SystemExit(f"checksum mismatch for {dest}")
    if record is not None:
        record[os.path.relpath(dest, record["__root__"])] = {"url": url, "md5": md5_of(dest), "sha256": sha256_of(dest), "bytes": os.path.getsize(dest)}


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    out = sys.argv[1]
    if "--verify" in sys.argv:
        manifest = json.load(open(os.path.join(HERE, "assets.json")))
        bad = 0
        for group in manifest.values():
            for name, entry in group.items():
                for rel, info in entry["files"].items():
                    path = os.path.join(out, rel)
                    if not os.path.exists(path) or sha256_of(path) != info["sha256"]:
                        print("MISMATCH", rel)
                        bad += 1
        print("verified" if bad == 0 else f"{bad} file(s) differ")
        raise SystemExit(1 if bad else 0)
    manifest = {"textures": {}, "models": {}, "khronos": {}}
    for tex, role in TEXTURES.items():
        info = get_json(f"{API}/info/{tex}")
        files = get_json(f"{API}/files/{tex}")
        rec = {"__root__": out}
        for key, name in (("Diffuse", "diff"), ("nor_gl", "nor_gl"), ("arm", "arm")):
            entry = files[key]["1k"]["jpg"]
            ensure(entry["url"], os.path.join(out, "textures", tex, f"{tex}_{name}_1k.jpg"), entry.get("md5"), rec)
        rec.pop("__root__")
        manifest["textures"][tex] = {"role": role, "source": f"https://polyhaven.com/a/{tex}", "license": "CC0-1.0", "authors": sorted(info.get("authors", {}).keys()), "files": rec}
    for model in MODELS:
        info = get_json(f"{API}/info/{model}")
        files = get_json(f"{API}/files/{model}")
        entry = files["gltf"]["1k"]["gltf"]
        rec = {"__root__": out}
        ensure(entry["url"], os.path.join(out, "models", model, f"{model}.gltf"), entry.get("md5"), rec)
        for rel, inc in entry.get("include", {}).items():
            ensure(inc["url"], os.path.join(out, "models", model, rel), inc.get("md5"), rec)
        rec.pop("__root__")
        manifest["models"][model] = {"source": f"https://polyhaven.com/a/{model}", "license": "CC0-1.0", "authors": sorted(info.get("authors", {}).keys()), "files": rec}
    for name, lic in KHRONOS_MODELS.items():
        rec = {"__root__": out}
        ensure(f"{KHRONOS}/{name}/glTF-Binary/{name}.glb", os.path.join(out, "khronos", f"{name}.glb"), None, rec)
        ensure(f"{KHRONOS}/{name}/LICENSE.md", os.path.join(out, "khronos", f"{name}.LICENSE.md"), None, rec)
        rec.pop("__root__")
        manifest["khronos"][name] = {"source": f"https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/{name}", "license": lic,
                                     "attribution": {"CarConcept": "Car Concept, (c) Khronos Group, created by Eric Chadwick of Darmstadt Graphics Group GmbH, 2024, CC BY 4.0 International",
                                                     "CesiumMan": "Cesium Man, (c) Cesium, CC BY 4.0 International"}[name], "files": rec}
    with open(os.path.join(out, "assets.json"), "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
    print(f"wrote {os.path.join(out, 'assets.json')}: {len(manifest['textures'])} textures, {len(manifest['models'])} models, {len(manifest['khronos'])} Khronos models")


if __name__ == "__main__":
    main()
