import json
import os
import struct
import wave
from typing import Optional

from .models import AudioMetadata, ModelMetadata, TextureMetadata


def classify_pbr_map(filename: str) -> str:
    low = os.path.basename(filename).lower()
    if any(k in low for k in ("albedo", "basecolor", "base_color", "diffuse", "_col", "_diff")):
        return "base_color"
    if any(k in low for k in ("normal", "_nor_gl", "_nor_dx", "_nrm")):
        return "normal"
    if any(k in low for k in ("roughness", "_rough", "_rgh")):
        return "roughness"
    if any(k in low for k in ("metallic", "_metal", "_met")):
        return "metallic"
    if any(k in low for k in ("_ao", "ambientocclusion", "ambient_occlusion", "occlusion")):
        return "ambient_occlusion"
    if any(k in low for k in ("height", "displacement", "_disp", "_bump")):
        return "height"
    if any(k in low for k in ("opacity", "alpha", "transparency")):
        return "opacity"
    if any(k in low for k in ("emissive", "emission", "_emit")):
        return "emissive"
    if low.endswith((".exr", ".hdr")):
        return "hdri_environment"
    return "unknown"


def _extract_gltf_json_metadata(gltf: dict, fmt: str) -> ModelMetadata:
    meta = ModelMetadata(file_format=fmt)
    meshes = gltf.get("meshes", [])
    meta.mesh_count = len(meshes)
    meta.node_count = len(gltf.get("nodes", []))
    meta.detected_up_axis = "Y-up (glTF 2.0 standard; rotate +90 deg X for SAIGE Z-up)"

    accessors = gltf.get("accessors", [])
    total_tris = 0
    bmin = [float("inf"), float("inf"), float("inf")]
    bmax = [float("-inf"), float("-inf"), float("-inf")]

    for mesh in meshes:
        for prim in mesh.get("primitives", []):
            idx_acc = prim.get("indices")
            if isinstance(idx_acc, int) and 0 <= idx_acc < len(accessors):
                total_tris += int(accessors[idx_acc].get("count", 0)) // 3
            pos_acc = prim.get("attributes", {}).get("POSITION")
            if isinstance(pos_acc, int) and 0 <= pos_acc < len(accessors):
                acc = accessors[pos_acc]
                if idx_acc is None:
                    total_tris += int(acc.get("count", 0)) // 3
                amn = acc.get("min")
                amx = acc.get("max")
                if isinstance(amn, list) and len(amn) == 3:
                    bmin = [min(bmin[i], float(amn[i])) for i in range(3)]
                if isinstance(amx, list) and len(amx) == 3:
                    bmax = [max(bmax[i], float(amx[i])) for i in range(3)]

    meta.triangle_count = total_tris
    if bmin[0] != float("inf") and bmax[0] != float("-inf"):
        meta.bbox_min = bmin
        meta.bbox_max = bmax

    skins = gltf.get("skins", [])
    if skins:
        meta.has_rig = True
        nodes = gltf.get("nodes", [])
        for j_idx in skins[0].get("joints", []):
            if 0 <= j_idx < len(nodes):
                meta.bone_names.append(nodes[j_idx].get("name", f"joint_{j_idx}"))

    for anim in gltf.get("animations", []):
        meta.animation_names.append(anim.get("name", "unnamed_anim"))
    return meta


def extract_model_metadata(path: str) -> Optional[ModelMetadata]:
    ext = os.path.splitext(path)[1].lower()
    if ext == ".glb":
        with open(path, "rb") as f:
            header = f.read(12)
            if len(header) < 12:
                return None
            magic, _version, _length = struct.unpack("<III", header)
            if magic != 0x46546C67:  # "glTF"
                return None
            chunk_hdr = f.read(8)
            if len(chunk_hdr) < 8:
                return None
            chunk_len, chunk_type = struct.unpack("<II", chunk_hdr)
            if chunk_type != 0x4E4F534A:  # "JSON"
                return None
            gltf = json.loads(f.read(chunk_len).decode("utf-8"))
        return _extract_gltf_json_metadata(gltf, "GLB")

    if ext == ".gltf":
        with open(path, "r", encoding="utf-8") as f:
            gltf = json.load(f)
        return _extract_gltf_json_metadata(gltf, "glTF")

    if ext == ".obj":
        tris = 0
        bmin = [float("inf"), float("inf"), float("inf")]
        bmax = [float("-inf"), float("-inf"), float("-inf")]
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            for line in f:
                if line.startswith("v "):
                    parts = line.split()
                    if len(parts) >= 4:
                        v = [float(parts[1]), float(parts[2]), float(parts[3])]
                        bmin = [min(bmin[i], v[i]) for i in range(3)]
                        bmax = [max(bmax[i], v[i]) for i in range(3)]
                elif line.startswith("f "):
                    verts = len(line.split()) - 1
                    if verts >= 3:
                        tris += verts - 2
        meta = ModelMetadata(file_format="OBJ", triangle_count=tris, mesh_count=1)
        if bmin[0] != float("inf"):
            meta.bbox_min = bmin
            meta.bbox_max = bmax
        return meta

    return None


def extract_audio_metadata(path: str) -> Optional[AudioMetadata]:
    ext = os.path.splitext(path)[1].lower()
    if ext == ".wav":
        with wave.open(path, "rb") as w:
            sr = w.getframerate()
            ch = w.getnchannels()
            sw = w.getsampwidth() * 8
            nf = w.getnframes()
            dur = (float(nf) / float(sr)) if sr > 0 else 0.0
            return AudioMetadata(
                file_format="WAV",
                sample_rate_hz=sr,
                channels=ch,
                sample_width_bits=sw,
                frame_count=nf,
                duration_sec=round(dur, 4),
            )
    return None


def extract_texture_metadata(path: str) -> Optional[TextureMetadata]:
    ext = os.path.splitext(path)[1].lower()
    pbr = classify_pbr_map(path)
    if ext == ".png":
        with open(path, "rb") as f:
            sig = f.read(25)
            if len(sig) == 25 and sig[:8] == b"\x89PNG\r\n\x1a\n" and sig[12:16] == b"IHDR":
                w, h, bpp = struct.unpack(">IIB", sig[16:25])
                return TextureMetadata(
                    file_format="PNG", width=w, height=h, bit_depth=bpp, pbr_map_type=pbr
                )
    if ext == ".exr":
        with open(path, "rb") as f:
            magic = f.read(4)
            if magic == b"\x76\x2f\x31\x01":
                return TextureMetadata(file_format="EXR", bit_depth=16, pbr_map_type=pbr)
    if ext == ".hdr":
        w, h = 0, 0
        with open(path, "rb") as f:
            for _ in range(32):
                line = f.readline().decode("ascii", errors="ignore").strip()
                if line.startswith("-Y ") and "+X " in line:
                    parts = line.split()
                    if len(parts) >= 4:
                        h = int(parts[1])
                        w = int(parts[3])
                    break
        return TextureMetadata(file_format="HDR", width=w, height=h, bit_depth=32, pbr_map_type=pbr)
    return None
