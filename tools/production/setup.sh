#!/usr/bin/env bash
# Reproducible setup of the SAIGE production toolchain on Ubuntu 24.04 (cloud sessions, CI, a developer machine). Idempotent; safe to re-run.
#   tools/production/setup.sh [--no-apt] [--no-blender] [--no-npm] [--no-python]
# Installs: SoX, Opus/FLAC/Vorbis/LAME encoders, ImageMagick, libvips, OpenEXR, optipng, pngquant, webp, assimp, Blender (headless Python), glTF Transform,
# meshoptimizer/gltfpack, a Basis/KTX2 encoder (WASM, via npm: no GitHub release download needed), and a Python venv with the asset libraries.
# Everything installs under the user's tools home ($SAIGE_TOOLS_HOME, default ~/.saige-tools) except apt packages. Needs only: Ubuntu archive, PyPI and npm.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TOOLS_HOME=${SAIGE_TOOLS_HOME:-$HOME/.saige-tools}
APT=1; BLENDER=1; NPM=1; PY=1
for a in "$@"; do case $a in --no-apt) APT=0;; --no-blender) BLENDER=0;; --no-npm) NPM=0;; --no-python) PY=0;; *) echo "unknown option $a" >&2; exit 2;; esac; done
mkdir -p "$TOOLS_HOME"
SUDO=""; [ "$(id -u)" -ne 0 ] && SUDO="sudo"

if [ $APT -eq 1 ] && command -v apt-get >/dev/null; then
    pkgs=(sox libsox-fmt-all opus-tools flac vorbis-tools lame imagemagick libvips-tools openexr optipng pngquant webp assimp-utils p7zip-full jq ffmpeg git-lfs)
    [ $BLENDER -eq 1 ] && pkgs+=(blender)
    $SUDO apt-get update -qq
    nice -n 19 $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${pkgs[@]}"
fi
if [ $NPM -eq 1 ]; then
    command -v npm >/dev/null || { echo "npm is required (Node 20+)" >&2; exit 1; }
    mkdir -p "$TOOLS_HOME/node"; cp "$HERE/package.json" "$TOOLS_HOME/node/package.json"
    (cd "$TOOLS_HOME/node" && npm install --no-audit --no-fund --loglevel=error)
    # ES modules ignore NODE_PATH: the scripts in prep/ resolve their packages through this link
    ln -sfn "$TOOLS_HOME/node/node_modules" "$HERE/node_modules"
fi
if [ $PY -eq 1 ]; then
    [ -x "$TOOLS_HOME/venv/bin/python" ] || python3 -m venv "$TOOLS_HOME/venv"
    "$TOOLS_HOME/venv/bin/pip" install -q --no-input -r "$HERE/requirements.txt"
fi
cat > "$TOOLS_HOME/env.sh" <<EOF
# source this file: puts the SAIGE production tools on PATH
export SAIGE_TOOLS_HOME="$TOOLS_HOME"
export PATH="$TOOLS_HOME/node/node_modules/.bin:$TOOLS_HOME/venv/bin:\$PATH"
export NODE_PATH="$TOOLS_HOME/node/node_modules"
export SAIGE_PRODUCTION_DIR="$HERE"
EOF
echo "done. 'source $TOOLS_HOME/env.sh' then 'python3 $HERE/doctor.py'"
