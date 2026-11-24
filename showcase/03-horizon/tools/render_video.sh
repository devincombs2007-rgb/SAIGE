#!/usr/bin/env bash
# Renders Showcase 03 with the real renderer in segments (each segment re-simulates from t = 0, which takes seconds), encodes each to H.264 and joins them.
#   render_video.sh BUILD_DIR ASSET_DIR OUT_DIR [WIDTH HEIGHT FPS]
# Segments that already exist in OUT_DIR are kept, so an interrupted run resumes.
set -euo pipefail
BUILD=${1:?build dir}; ASSETS=${2:?asset dir}; OUT=${3:?out dir}
W=${4:-1280}; H=${5:-720}; FPS=${6:-30}
SEGMENTS=(0:36 36:72 72:108 108:142)
mkdir -p "$OUT"
list="$OUT/segments.txt"; : > "$list"
for seg in "${SEGMENTS[@]}"; do
    a=${seg%%:*}; b=${seg##*:}
    file="$OUT/seg_${a}_${b}.mp4"
    echo "file '$file'" >> "$list"
    [ -s "$file" ] && continue
    fifo="$OUT/pipe_$a.ppm"; rm -f "$fifo"; mkfifo "$fifo"
    ffmpeg -loglevel error -y -f image2pipe -c:v ppm -framerate "$FPS" -i "$fifo" -c:v libx264 -preset slow -crf 17 -pix_fmt yuv420p -profile:v high -level 4.1 \
        -g "$FPS" -movflags +faststart "$file.part.mp4" &
    enc=$!
    "$BUILD/showcase/saige_horizon" --assets "$ASSETS" --out "$OUT/seg_${a}" --start "$a" --end "$b" --fps "$FPS" --width "$W" --height "$H" --frames-pipe "$fifo" > "$OUT/seg_${a}.log" 2>&1
    wait "$enc"
    mv "$file.part.mp4" "$file"
    rm -f "$fifo"
done
ffmpeg -loglevel error -y -f concat -safe 0 -i "$list" -c copy -movflags +faststart "$OUT/video_silent.mp4"
echo "wrote $OUT/video_silent.mp4"
