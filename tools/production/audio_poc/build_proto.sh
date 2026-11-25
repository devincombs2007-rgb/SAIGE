#!/usr/bin/env bash
# Builds audio_proto (the engine's audio model on miniaudio, offline) with the repository's own audio_model.cpp. Needs the third_party fetch first: ./fetch_miniaudio.sh. Then:
#   ./build_proto.sh OUT_BINARY && python3 check_proto.py OUT_BINARY WORKDIR      (check_proto needs numpy and soundfile: the production tools' venv has them)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$HERE/../../.." && pwd)
${CXX:-g++} -O2 -std=c++20 -I"$ROOT/include" -I"$HERE" "$HERE/audio_proto.cpp" "$ROOT/src/audio_model.cpp" -o "${1:?output binary}" -lpthread -ldl -lm
