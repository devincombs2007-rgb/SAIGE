#!/usr/bin/env python3
"""Regenerates every Showcase 01 asset (textures, glTF, scene.json) into ./assets. Requires numpy; nothing else."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scene  # noqa: E402
import textures  # noqa: E402

out = os.path.join(HERE, "assets")
textures.generate_all(out)
colliders, decals = scene.write_all(out)
print(f"wrote textures, courtyard.gltf, character.gltf, blob.gltf and scene.json ({colliders} colliders, {decals} decals) to {out}")
