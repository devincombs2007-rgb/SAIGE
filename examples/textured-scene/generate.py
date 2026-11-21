"""Rebuild SAIGE's original textured box fixture using the Python standard library."""
import base64
import json
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parent


def png():
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    rgba = [(232, 108, 37, 255), (34, 130, 186, 255)]
    rows = b"".join(
        b"\x00" + bytes(c for x in range(8) for c in rgba[(x // 2 + y // 2) % 2])
        for y in range(8)
    )
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", 8, 8, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def make_model():
    faces = [
        ((1, 0, 0), (0, 1, 0), (0, 0, 1)),
        ((-1, 0, 0), (0, -1, 0), (0, 0, 1)),
        ((0, 1, 0), (-1, 0, 0), (0, 0, 1)),
        ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
        ((0, 0, 1), (1, 0, 0), (0, 1, 0)),
        ((0, 0, -1), (0, 1, 0), (1, 0, 0)),
    ]
    corners = [(-1, -1, 0, 1), (1, -1, 1, 1), (1, 1, 1, 0), (-1, 1, 0, 0)]
    positions, normals, uv, indices = [], [], [], []
    for face, (normal, tangent, bitangent) in enumerate(faces):
        for s, t, u, v in corners:
            positions.extend(0.5 * (n + s * a + t * b) for n, a, b in zip(normal, tangent, bitangent))
            normals.extend(normal)
            uv.extend((u, v))
        indices.extend(4 * face + i for i in [0, 1, 2, 0, 2, 3])

    chunks = [struct.pack(f"<{len(data)}{fmt}", *data)
              for data, fmt in [(positions, "f"), (normals, "f"), (uv, "f"), (indices, "H")]]
    raw = b"".join(chunks)

    def uri(mime, data):
        return f"data:{mime};base64," + base64.b64encode(data).decode("ascii")

    model = {
        "asset": {"version": "2.0", "generator": "SAIGE original fixture"},
        "scene": 0, "scenes": [{"nodes": [0]}], "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                                    "indices": 3, "material": 0}]}],
        "materials": [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                                              "metallicFactor": 0, "roughnessFactor": 1}}],
        "textures": [{"source": 0}], "images": [{"uri": uri("image/png", png())}],
        "buffers": [{"byteLength": len(raw), "uri": uri("application/octet-stream", raw)}],
        "bufferViews": [{"buffer": 0, "byteOffset": sum(map(len, chunks[:i])), "byteLength": len(chunk)}
                        for i, chunk in enumerate(chunks)],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3",
             "min": [-0.5, -0.5, -0.5], "max": [0.5, 0.5, 0.5]},
            {"bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 24, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": 36, "type": "SCALAR"},
        ],
    }
    (ROOT / "checker-box.gltf").write_text(json.dumps(model, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    make_model()
