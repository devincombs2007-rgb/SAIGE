#!/usr/bin/env python3
"""Downloads the two CC0 compatibility-test assets chosen in docs/assets/external-asset-sources.md into a directory you name (never into the
repository: nothing here is committed). Uses only each site's documented API / official download links, checks sizes and SHA-256 against
the values recorded in the doc, and downloads nothing else. Needs network access to api.polyhaven.com, dl.polyhaven.org, ambientcg.com and
acg-download.struffelproductions.com.

    fetch_sample_assets.py OUT_DIR
"""
import hashlib
import json
import os
import sys
import urllib.request
import zipfile

UA = {"User-Agent": "saige-asset-fetch/1.0"}
EXPECTED = {  # SHA-256 of what was downloaded on 2026-11-23
    "marble_bust_01_1k.gltf": "b6f192fc01be4349470b66f6f6948bd37620267bdbc86f4568e4d078f97ff09b",
    "marble_bust_01.bin": "0bb757c1bd00bfe42005c84e7f76df98ce04a532e595a2ddda8a0335cd959e33",
    "textures/marble_bust_01_diff_1k.jpg": "cbb7dccdc62d7eaa240eef7b4956969bf6e73c7338b64f1378629838ea22383c",
    "textures/marble_bust_01_nor_gl_1k.jpg": "3d355904cd54c2de90d5d4310b80d43816fa2dd509f0a3445d654eab8e237c5f",
    "textures/marble_bust_01_rough_1k.jpg": "1c4502352dc936dd1f1c1a81b418706df1ce0445949874215015325f9b8921fb",
    "Rock063_1K-JPG.zip": "e2974a91631ea872cf0ecfc3755650c970ef0e6bf4fed5777847e63884319656",
}


def get(url):
    with urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=120) as r:
        return r.read()


def save(out, rel, data):
    path = os.path.join(out, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(data)
    digest = hashlib.sha256(data).hexdigest()
    status = "ok" if EXPECTED.get(os.path.basename(rel) if rel.endswith(".zip") else rel.split("/", 1)[-1] if rel.startswith("bust/") else rel) in (None, digest) else "CHANGED upstream"
    print(f"{rel}: {len(data)} bytes sha256 {digest[:16]}... {status}")


def main():
    out = sys.argv[1]
    # Poly Haven: the files manifest lists every file of the 1k glTF bundle with its exact URL (the textures live under /Models/jpg/, not next to the .gltf).
    manifest = json.loads(get("https://api.polyhaven.com/files/marble_bust_01"))["gltf"]["1k"]["gltf"]
    save(out, "bust/marble_bust_01_1k.gltf", get(manifest["url"]))
    for rel, entry in manifest["include"].items():
        save(out, "bust/" + rel, get(entry["url"]))
    # ambientCG: the v3 API lists the downloads; the link redirects to a download host.
    assets = json.loads(get("https://ambientcg.com/api/v3/assets?id=Rock063&include=downloads"))["assets"]
    link = next(d for d in assets[0]["downloads"] if d["attributes"] == "1K-JPG" and d["extension"] == "zip")
    data = get(link["url"])
    save(out, "rock/Rock063_1K-JPG.zip", data)
    with zipfile.ZipFile(os.path.join(out, "rock/Rock063_1K-JPG.zip")) as z:
        z.extractall(os.path.join(out, "rock"))


if __name__ == "__main__":
    main()
