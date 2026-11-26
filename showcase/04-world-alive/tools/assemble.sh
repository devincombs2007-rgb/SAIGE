#!/usr/bin/env bash
# Showcase 04 assembly: joins the rendered segments, adds the title and the end card (attributions and disclosure), loudness-normalises the spatial mix and writes an
# iPhone-compatible MP4 (H.264 High, yuv420p, AAC-LC, faststart).
#   assemble.sh SEGMENT_DIR AUDIO.wav OUT.mp4 [WIDTH HEIGHT FPS] [DISCLOSURE_LINE]
set -euo pipefail
SEG=${1:?segment dir}; A=${2:?audio wav}; OUT=${3:?output}
W=${4:-1920}; H=${5:-1080}; FPS=${6:-30}
DISC=${7:-"Rendered offline by the SAIGE engine on a software Vulkan rasterizer (llvmpipe, CPU only)"}
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
FONTB=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
TMP=$(mktemp -d)
ls "$SEG"/seg_*.mp4 | sort | sed "s|^|file '|; s|\$|'|" > "$TMP/list.txt"
ffmpeg -loglevel error -y -f concat -safe 0 -i "$TMP/list.txt" -c copy "$TMP/silent.mp4"
V="$TMP/silent.mp4"
DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$V")
CARD=11
TOTAL=$(python3 -c "print($DUR+$CARD)")
big=$((H/9)); mid=$((H/28)); small=$((H/46))
L=("A WORLD ALIVE"
"A coastal district simulated live: 60 residents, signalled traffic, an accident and the response. Every shot is the engine."
"$DISC"
"Video: ${W}x${H}, ${FPS} distinct frames per second, offline capture. Not real-time performance, no 60 FPS claim. Software rendering is not GPU evidence."
"Car Concept (c) Khronos Group, Eric Chadwick of Darmstadt Graphics Group GmbH, CC BY 4.0"
"Universal Animation Library, Quaternius, CC0"
"Textures and props from Poly Haven, CC0 (polyhaven.com)"
"Sound: CC0 recordings from OpenGameArt; spatial mix (panning, distance, Doppler, indoor filtering) computed from the simulation's own event log"
"Not in this film: destruction and ragdoll physics (Jolt adapter), boat buoyancy simulation, point lights (emissive windows and lamps with bloom instead)")
for i in "${!L[@]}"; do printf %s "${L[$i]}" > "$TMP/line$i.txt"; done
# Programme loudness -16 LUFS, true peak -1.5 dB.
ffmpeg -loglevel error -y -i "$A" -af "loudnorm=I=-16:TP=-1.5:LRA=11" -ar 48000 "$TMP/audio.wav"
ffmpeg -loglevel error -y -i "$V" -i "$TMP/audio.wav" \
  -f lavfi -t $CARD -i "color=c=0x05070b:s=${W}x${H}:r=${FPS}" \
  -filter_complex "
   [0:v]fade=t=in:st=0:d=1.2,fade=t=out:st=$(python3 -c "print($DUR-1.6)"):d=1.6,
     drawtext=fontfile=$FONTB:text='SAIGE':fontsize=$big:fontcolor=white:x=(w-text_w)/2:y=h*0.36:enable='between(t,1.6,6.4)':alpha='if(lt(t,2.4),(t-1.6)/0.8,if(gt(t,5.6),(6.4-t)/0.8,1))',
     drawtext=fontfile=$FONT:text='A   W O R L D   A L I V E':fontsize=$mid:fontcolor=white:x=(w-text_w)/2:y=h*0.36+$big*1.25:enable='between(t,2.0,6.4)':alpha='if(lt(t,2.8),(t-2.0)/0.8,if(gt(t,5.6),(6.4-t)/0.8,1))'[main];
   [2:v]drawtext=fontfile=$FONTB:textfile=$TMP/line0.txt:fontsize=$mid:fontcolor=white:x=(w-text_w)/2:y=h*0.10,
     drawtext=fontfile=$FONT:textfile=$TMP/line1.txt:fontsize=$small:fontcolor=0xc8d0dc:x=(w-text_w)/2:y=h*0.18,
     drawtext=fontfile=$FONT:textfile=$TMP/line2.txt:fontsize=$small:fontcolor=0xc8d0dc:x=(w-text_w)/2:y=h*0.23,
     drawtext=fontfile=$FONT:textfile=$TMP/line3.txt:fontsize=$small:fontcolor=0xffd479:x=(w-text_w)/2:y=h*0.28,
     drawtext=fontfile=$FONT:textfile=$TMP/line4.txt:fontsize=$small:fontcolor=white:x=(w-text_w)/2:y=h*0.42,
     drawtext=fontfile=$FONT:textfile=$TMP/line5.txt:fontsize=$small:fontcolor=white:x=(w-text_w)/2:y=h*0.47,
     drawtext=fontfile=$FONT:textfile=$TMP/line6.txt:fontsize=$small:fontcolor=white:x=(w-text_w)/2:y=h*0.52,
     drawtext=fontfile=$FONT:textfile=$TMP/line7.txt:fontsize=$small:fontcolor=0xc8d0dc:x=(w-text_w)/2:y=h*0.60,
     drawtext=fontfile=$FONT:textfile=$TMP/line8.txt:fontsize=$small:fontcolor=0xc8d0dc:x=(w-text_w)/2:y=h*0.65,
     fade=t=in:st=0:d=0.8,fade=t=out:st=$(python3 -c "print($CARD-1)"):d=1[card];
   [main][card]concat=n=2:v=1:a=0,format=yuv420p[v];
   [1:a]aresample=48000,afade=t=in:st=0:d=1.5,apad=whole_dur=$TOTAL,atrim=0:$TOTAL,afade=t=out:st=$(python3 -c "print($DUR-2)"):d=2[a]" \
  -map "[v]" -map "[a]" -c:v libx264 -preset slow -crf 20 -profile:v high -level 4.2 -pix_fmt yuv420p -r "$FPS" -g "$FPS" -c:a aac -b:a 192k -movflags +faststart "$OUT"
ffprobe -v error -show_entries stream=codec_name,width,height,r_frame_rate,pix_fmt,profile:format=duration,size -of default=nw=1 "$OUT"
rm -rf "$TMP"
