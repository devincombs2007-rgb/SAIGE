#!/usr/bin/env bash
# Adds the title, the end card with the attributions and the disclosure, and the soundtrack to the silent render, and writes an iPhone-compatible MP4
# (H.264 High, yuv420p, AAC-LC, faststart).
#   assemble.sh VIDEO_SILENT.mp4 AUDIO.wav OUT.mp4 [WIDTH HEIGHT FPS] [DISCLOSURE_LINE]
set -euo pipefail
V=${1:?silent video}; A=${2:?audio wav}; OUT=${3:?output}
W=${4:-1920}; H=${5:-1080}; FPS=${6:-30}
DISC=${7:-"Rendered offline by the SAIGE engine on a software Vulkan rasterizer (llvmpipe, CPU only)"}
FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
FONTB=/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf
DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$V")
CARD=7
TOTAL=$(python3 -c "print($DUR+$CARD)")
big=$((H/9)); mid=$((H/28)); small=$((H/42))
TMP=$(mktemp -d)
printf %s "BEYOND THE HORIZON" > "$TMP/line0.txt"
printf %s "Drawn by the SAIGE renderer: PBR, HDR, cascaded sun shadows, sky with sun, moon and stars, AO, bloom, FXAA, GPU skinning" > "$TMP/line1.txt"
printf %s "$DISC" > "$TMP/line2.txt"
printf %s "Video: ${W}x${H}, ${FPS} distinct frames per second, offline capture. Not real-time performance and no 60 FPS claim." > "$TMP/line3.txt"
printf %s "Car Concept (c) Khronos Group, Eric Chadwick of Darmstadt Graphics Group GmbH, CC BY 4.0" > "$TMP/line4.txt"
printf %s "Cesium Man (c) Cesium, CC BY 4.0" > "$TMP/line5.txt"
printf %s "Textures and props from Poly Haven, CC0 (polyhaven.com)" > "$TMP/line6.txt"
printf %s "Sound: CC0 recordings from OpenGameArt (domasx2, qubodup, Fantozzi, rubberduck, tinyworlds, ignasd); pad and ambience generated" > "$TMP/line7.txt"
printf %s "Not implemented: point lights (emissive windows and lamps with bloom instead), pedestrians, a seated driver pose, spatial audio (the mix is stereo)" > "$TMP/line8.txt"
ffmpeg -loglevel error -y -i "$V" -i "$A" \
  -f lavfi -t $CARD -i "color=c=0x05070b:s=${W}x${H}:r=${FPS}" \
  -filter_complex "
   [0:v]fade=t=in:st=0:d=1.2,fade=t=out:st=$(python3 -c "print($DUR-1.6)"):d=1.6,
     drawtext=fontfile=$FONTB:text='SAIGE':fontsize=$big:fontcolor=white:x=(w-text_w)/2:y=h*0.36:enable='between(t,1.6,6.4)':alpha='if(lt(t,2.4),(t-1.6)/0.8,if(gt(t,5.6),(6.4-t)/0.8,1))',
     drawtext=fontfile=$FONT:text='B E Y O N D   T H E   H O R I Z O N':fontsize=$mid:fontcolor=white:x=(w-text_w)/2:y=h*0.36+$big*1.25:enable='between(t,2.0,6.4)':alpha='if(lt(t,2.8),(t-2.0)/0.8,if(gt(t,5.6),(6.4-t)/0.8,1))'[main];
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
   [1:a]aresample=48000,apad=whole_dur=$TOTAL,atrim=0:$TOTAL[a]" \
  -map "[v]" -map "[a]" -c:v libx264 -preset slow -crf 21 -profile:v high -level 4.2 -pix_fmt yuv420p -r "$FPS" -g "$FPS" -c:a aac -b:a 192k -movflags +faststart "$OUT"
ffprobe -v error -show_entries stream=codec_name,width,height,r_frame_rate,pix_fmt,profile:format=duration,size -of default=nw=1 "$OUT"
