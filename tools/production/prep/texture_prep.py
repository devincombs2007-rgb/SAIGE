#!/usr/bin/env python3
"""PBR texture preparation: normalise a material's maps into SAIGE's layout.

  texture_prep.py --out DIR [--basecolor F] [--normal F --normal-convention gl|dx] [--roughness F] [--metalness F] [--ao F] [--orm F] [--emissive F] [--max-size 1024] [--format jpg|png] [--name NAME]

SAIGE's material slots (docs/rendering/material-architecture.md): base color (sRGB), metallic-roughness (linear; G roughness, B metallic: glTF's layout), normal (linear, +Y up = OpenGL/glTF),
occlusion (linear, R), emissive (sRGB). So: separate roughness/metalness/AO maps are packed into one ORM image (R AO, G roughness, B metalness); a DirectX normal map (green down) is
converted to OpenGL by inverting green; every map is resized to at most --max-size on its long side by Lanczos (kept power-of-two when the source was) and saved as PNG (normals, ORM) or
JPEG quality 92 (base color, emissive). Writes a JSON manifest of what was produced and the GPU memory it costs.
"""
import argparse, json
from pathlib import Path
import numpy as np
from PIL import Image


def load(path, mode):
    im = Image.open(path)
    if mode == "L":
        im = im.convert("L")
    else:
        im = im.convert("RGB")
    return im


def fit(im, max_size):
    w, h = im.size
    s = min(1.0, max_size / max(w, h))
    if s < 1.0:
        im = im.resize((max(1, round(w * s)), max(1, round(h * s))), Image.LANCZOS)
    return im


def main():
    ap = argparse.ArgumentParser()
    for k in ("basecolor", "normal", "roughness", "metalness", "ao", "orm", "emissive"):
        ap.add_argument("--" + k)
    ap.add_argument("--normal-convention", default="gl", choices=["gl", "dx"])
    ap.add_argument("--max-size", type=int, default=1024); ap.add_argument("--format", default="jpg", choices=["jpg", "png"])
    ap.add_argument("--out", required=True); ap.add_argument("--name", default="material")
    a = ap.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    produced = {}
    mem = 0

    def save(name, im, fmt):
        nonlocal mem
        path = out / f"{a.name}_{name}.{fmt}"
        if fmt == "jpg":
            im.save(path, quality=92, optimize=True)
        else:
            im.save(path, optimize=True)
        produced[name] = {"file": path.name, "size": list(im.size), "bytes": path.stat().st_size}
        mem += int(im.size[0] * im.size[1] * 4 * 4 / 3)

    if a.basecolor:
        save("basecolor", fit(load(a.basecolor, "RGB"), a.max_size), a.format)
    if a.normal:
        n = fit(load(a.normal, "RGB"), a.max_size)
        arr = np.asarray(n).copy()
        if a.normal_convention == "dx":
            arr[..., 1] = 255 - arr[..., 1]
        save("normal", Image.fromarray(arr), "png")
    if a.orm:
        save("orm", fit(load(a.orm, "RGB"), a.max_size), "png")
    elif a.roughness or a.metalness or a.ao:
        ref = next(p for p in (a.roughness, a.metalness, a.ao) if p)
        size = fit(load(ref, "L"), a.max_size).size

        def chan(p, default):
            if p:
                return np.asarray(fit(load(p, "L"), a.max_size).resize(size, Image.LANCZOS))
            return np.full((size[1], size[0]), default, dtype=np.uint8)
        orm = np.stack([chan(a.ao, 255), chan(a.roughness, 255), chan(a.metalness, 0)], axis=-1)
        save("orm", Image.fromarray(orm), "png")
    if a.emissive:
        save("emissive", fit(load(a.emissive, "RGB"), a.max_size), a.format)
    manifest = {"name": a.name, "maps": produced, "gpu_bytes_rgba8_mips": mem, "layout": {"orm": "R occlusion, G roughness, B metalness (linear)", "normal": "OpenGL (+Y up), linear", "basecolor": "sRGB"}}
    (out / f"{a.name}.json").write_text(json.dumps(manifest, indent=1))
    print(json.dumps(manifest, indent=1))


main()
