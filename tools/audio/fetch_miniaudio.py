#!/usr/bin/env python3
"""Fetches miniaudio (one header, public domain / MIT-0 dual licence) and the stb_vorbis decoder it ships in extras/ (v1.22, public domain / MIT dual licence) at a pinned tag into DIR, each verified against a
pinned SHA-256, for the optional device adapter (engine/audio_miniaudio).

  python3 tools/audio/fetch_miniaudio.py DIR           # then configure with -DSAIGE_AUDIO_MINIAUDIO=ON -DSAIGE_MINIAUDIO_ROOT=DIR

Only raw.githubusercontent.com is needed. A mismatch means upstream changed or the download was damaged: review, then update the pin on purpose. Nothing is committed to the repository.
"""
import hashlib, sys, urllib.request
from pathlib import Path

TAG = "0.11.26"
FILES = {  # destination name -> (path in the repository at the tag, SHA-256)
    "miniaudio.h": ("miniaudio.h", "6b376c7fbb1352ba1e0e432ab66069b335ea9405e45cb6c30b97ef0f3c9c3e0e"),
    "stb_vorbis.c": ("extras/stb_vorbis.c", "4c7cb2ff1f7011e9d67950446b7eb9ca044f2e464d76bfbb0b84dd2e23e65636"),
}
URL = "https://raw.githubusercontent.com/mackron/miniaudio/{tag}/{path}"


def fetch(path):
    last = None
    for _ in range(4):
        try:
            with urllib.request.urlopen(URL.format(tag=TAG, path=path), timeout=60) as r:
                return r.read()
        except Exception as e:  # network hiccup: retry
            last = e
    sys.exit(f"{path}: {last}")


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for name, (path, sha) in FILES.items():
        dest = out / name
        if dest.exists() and hashlib.sha256(dest.read_bytes()).hexdigest() == sha:
            continue
        data = fetch(path)
        got = hashlib.sha256(data).hexdigest()
        if got != sha:
            sys.exit(f"checksum mismatch for {name} at {TAG}: {got} (expected {sha})")
        dest.write_bytes(data)
    print(f"miniaudio {TAG} and stb_vorbis ready in {out}")


if __name__ == "__main__":
    main()
