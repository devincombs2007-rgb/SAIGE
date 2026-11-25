#!/usr/bin/env python3
"""Fetch and verify the small CC0 asset selection in assets.json."""
import hashlib
import json
import subprocess
import sys
from pathlib import Path

root = Path(sys.argv[1])
root.mkdir(parents=True, exist_ok=True)
manifest = json.loads(Path(__file__).with_name("assets.json").read_text())
for entry in manifest.values():
    for relative, remote in entry["files"].items():
        output = root / relative
        output.parent.mkdir(parents=True, exist_ok=True)
        if not output.exists() or hashlib.md5(output.read_bytes()).hexdigest() != remote["md5"]:
            subprocess.run(["curl", "--fail", "--silent", "--show-error", "--location", remote["url"], "-o", str(output)], check=True)
        if hashlib.md5(output.read_bytes()).hexdigest() != remote["md5"]:
            raise RuntimeError(f"Asset checksum mismatch: {relative}")
