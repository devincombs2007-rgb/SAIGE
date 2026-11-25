#!/usr/bin/env bash
# Fetches miniaudio (public domain / MIT-0) and its reverb node (extras) into audio_poc/third_party, checking pinned SHA-256 sums. Only raw.githubusercontent.com is needed.
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
TP="$HERE/third_party/miniaudio"
BASE=https://raw.githubusercontent.com/mackron/miniaudio/master
mkdir -p "$TP/extras/nodes/ma_reverb_node"
get() { # url dest sha256
    [ -f "$2" ] && echo "$3  $2" | sha256sum -c --quiet - 2>/dev/null && return 0
    curl -fsSL "$1" -o "$2"
    echo "$3  $2" | sha256sum -c --quiet - || { echo "checksum mismatch for $2 (upstream changed: review the new version, then update the pin)" >&2; exit 1; }
}
get $BASE/miniaudio.h "$TP/miniaudio.h" 6b376c7fbb1352ba1e0e432ab66069b335ea9405e45cb6c30b97ef0f3c9c3e0e
for f in "ma_reverb_node.c aa0d4198c1bc616bec8cba0c996ab87bbd187d480b26c16626485a9be558668b" "ma_reverb_node.h da13fa10252117c0e818d3b5f98a0346b4e84347049d0eebdffa2b31c6c824fb" "verblib.h 486ba61d414ab1bbb32060c05228a8563ad953e4b548e5b593e145cc126faf8a"; do
    set -- $f
    get $BASE/extras/nodes/ma_reverb_node/$1 "$TP/extras/nodes/ma_reverb_node/$1" $2
done
echo "miniaudio v0.11.26 (2026-10-31) ready in $TP"
