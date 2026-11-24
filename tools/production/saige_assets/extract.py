"""Technical metadata of an asset file, extracted without the engine: what a catalog record stores so that a model's cost and shape are known before it is imported.

Models (.glb/.gltf): triangle and vertex counts, materials, textures with their GPU memory estimate, animations (names, durations), skins (joint counts), extensions, bounds.
Images: size, mode, alpha, mip-chain memory. HDRIs: size. Audio: duration, sample rate, channels, peak, integrated loudness, silence, loop seam.
"""
import json, math, os, struct, subprocess
from pathlib import Path

GLB_MAGIC = 0x46546C67


def read_gltf_json(path):
    path = Path(path)
    data = path.read_bytes()
    if path.suffix.lower() == ".glb":
        magic, version, length = struct.unpack_from("<III", data, 0)
        if magic != GLB_MAGIC:
            raise ValueError("not a GLB")
        off = 12
        js, bin_chunk = None, b""
        while off + 8 <= len(data):
            clen, ctype = struct.unpack_from("<II", data, off)
            body = data[off + 8: off + 8 + clen]
            if ctype == 0x4E4F534A:
                js = json.loads(body)
            elif ctype == 0x004E4942:
                bin_chunk = body
            off += 8 + clen
        return js, bin_chunk
    return json.loads(data), None


def _image_size(path_or_bytes):
    from PIL import Image
    import io
    try:
        im = Image.open(io.BytesIO(path_or_bytes) if isinstance(path_or_bytes, (bytes, bytearray)) else path_or_bytes)
        return im.size, im.mode
    except Exception:
        return None, None


def describe_gltf(path):
    js, bin_chunk = read_gltf_json(path)
    path = Path(path)
    acc = js.get("accessors", [])
    views = js.get("bufferViews", [])
    tris = verts = 0
    prim_count = 0
    mins, maxs = [1e30] * 3, [-1e30] * 3
    skinned_prims = 0
    for mesh in js.get("meshes", []):
        for prim in mesh.get("primitives", []):
            prim_count += 1
            attrs = prim.get("attributes", {})
            if "POSITION" in attrs:
                a = acc[attrs["POSITION"]]
                verts += a.get("count", 0)
                if a.get("min") and a.get("max"):
                    for i in range(3):
                        mins[i] = min(mins[i], a["min"][i])
                        maxs[i] = max(maxs[i], a["max"][i])
            idx = prim.get("indices")
            if idx is not None:
                tris += acc[idx].get("count", 0) // 3
            elif "POSITION" in attrs:
                tris += acc[attrs["POSITION"]].get("count", 0) // 3
            if "JOINTS_0" in attrs:
                skinned_prims += 1
    textures = []
    tex_bytes = 0
    for i, img in enumerate(js.get("images", [])):
        size = None
        mode = None
        if "bufferView" in img and bin_chunk is not None:
            v = views[img["bufferView"]]
            blob = bin_chunk[v.get("byteOffset", 0): v.get("byteOffset", 0) + v["byteLength"]]
            if img.get("mimeType") == "image/ktx2":
                size = None
            else:
                size, mode = _image_size(blob)
        elif "uri" in img and not img["uri"].startswith("data:"):
            p = path.parent / img["uri"]
            if p.exists():
                size, mode = _image_size(str(p))
        mem = int(size[0] * size[1] * 4 * 4 / 3) if size else 0
        tex_bytes += mem
        textures.append({"index": i, "name": img.get("name"), "size": list(size) if size else None, "mime": img.get("mimeType"), "gpu_bytes_rgba8_mips": mem})
    anims = []
    for a in js.get("animations", []):
        dur = 0.0
        for s in a.get("samplers", []):
            inp = acc[s["input"]]
            if inp.get("max"):
                dur = max(dur, float(inp["max"][0]))
        anims.append({"name": a.get("name"), "duration_s": round(dur, 3), "channels": len(a.get("channels", []))})
    skins = [{"name": s.get("name"), "joints": len(s.get("joints", []))} for s in js.get("skins", [])]
    names = [n.get("name", "") for n in js.get("nodes", [])]
    return {
        "format": path.suffix.lower().lstrip("."),
        "generator": js.get("asset", {}).get("generator"),
        "meshes": len(js.get("meshes", [])), "primitives": prim_count, "triangles": tris, "vertices": verts,
        "nodes": len(js.get("nodes", [])), "materials": len(js.get("materials", [])), "textures": textures, "texture_gpu_bytes": tex_bytes,
        "animations": anims, "skins": skins, "skinned_primitives": skinned_prims,
        "extensions_used": js.get("extensionsUsed", []), "extensions_required": js.get("extensionsRequired", []),
        "bounds_min": mins if verts else None, "bounds_max": maxs if verts else None,
        "node_names_sample": names[:12],
    }


def describe_image(path):
    from PIL import Image
    im = Image.open(path)
    w, h = im.size
    return {"format": im.format, "size": [w, h], "mode": im.mode, "has_alpha": "A" in im.mode, "gpu_bytes_rgba8_mips": int(w * h * 4 * 4 / 3), "pot": (w & (w - 1)) == 0 and (h & (h - 1)) == 0}


def describe_hdr(path):
    p = Path(path)
    out = {"format": p.suffix.lower().lstrip(".")}
    try:
        if p.suffix.lower() == ".hdr":
            with open(p, "rb") as f:
                head = f.read(256).decode("latin1")
            import re
            m = re.search(r"-Y (\d+) \+X (\d+)", head)
            if m:
                out["size"] = [int(m.group(2)), int(m.group(1))]
        elif p.suffix.lower() == ".exr":
            out["size"] = None
    except Exception:
        pass
    return out


def describe_audio(path, analyse=True):
    import numpy as np
    import soundfile as sf
    path = str(path)
    try:
        data, sr = sf.read(path, always_2d=True, dtype="float32")
    except Exception:
        # fall back to ffmpeg decode for formats libsndfile lacks
        import tempfile
        tmp = tempfile.mktemp(suffix=".wav")
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", path, "-ac", "2", tmp], check=True)
        data, sr = sf.read(tmp, always_2d=True, dtype="float32")
        os.unlink(tmp)
    dur = len(data) / sr
    peak = float(np.max(np.abs(data))) if data.size else 0.0
    out = {"duration_s": round(dur, 3), "sample_rate": sr, "channels": data.shape[1], "peak_dbfs": round(20 * math.log10(max(peak, 1e-9)), 2), "bytes_pcm16": int(len(data) * data.shape[1] * 2)}
    if analyse and dur > 0.05:
        mono = data.mean(axis=1)
        try:
            import pyloudnorm as pyln
            meter = pyln.Meter(sr)
            if dur >= 0.4:
                out["lufs_integrated"] = round(float(meter.integrated_loudness(data if data.shape[1] <= 2 else mono)), 2)
        except Exception:
            pass
        thr = 10 ** (-50 / 20)
        out["silence_ratio"] = round(float(np.mean(np.abs(mono) < thr)), 3)
        n = min(len(mono) // 8, int(0.05 * sr))
        if n > 16:
            head, tail = mono[:n], mono[-n:]
            out["loop_seam_discontinuity"] = round(float(abs(mono[0] - mono[-1])), 4)
            out["loop_edge_rms_ratio"] = round(float((np.sqrt(np.mean(head ** 2)) + 1e-9) / (np.sqrt(np.mean(tail ** 2)) + 1e-9)), 3)
    return out


def describe(path):
    p = Path(path)
    ext = p.suffix.lower()
    try:
        if ext in (".glb", ".gltf"):
            return describe_gltf(p)
        if ext in (".png", ".jpg", ".jpeg", ".webp", ".tga", ".bmp", ".tif", ".tiff"):
            return describe_image(p)
        if ext in (".hdr", ".exr"):
            return describe_hdr(p)
        if ext in (".wav", ".flac", ".ogg", ".mp3", ".opus", ".aif", ".aiff", ".m4a"):
            return describe_audio(p)
    except Exception as e:  # noqa: BLE001
        return {"format": ext.lstrip("."), "error": str(e)[:200]}
    return {"format": ext.lstrip(".")}
