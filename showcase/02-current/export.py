#!/usr/bin/env python3
"""Stream real SAIGE PPM readback into a phone-compatible H.264 editorial cut."""
import argparse
import os
import subprocess
import tempfile
import time
import wave
from pathlib import Path

import numpy as np

def soundbed(path):
    rate = 48000
    t = np.arange(rate * 60) / rate
    signal = np.zeros_like(t)
    # Original, deliberately quiet ambient bed. Engine capture has no audio.
    for start, length, root in [(0, 19, 55.0), (12, 21, 61.7354), (28, 20, 48.9994), (43, 17, 55.0)]:
        local = np.clip((t - start) / length, 0, 1)
        envelope = np.sin(np.pi * local) ** 2
        for partial, volume in [(1, .31), (1.5, .17), (2, .12), (2.498, .08), (3.002, .045)]:
            signal += envelope * volume * np.sin(2 * np.pi * root * partial * t + partial)
    t_fade = np.minimum(1, np.minimum(t / 2.5, (60 - t) / 4.0))
    stereo = np.column_stack((signal, .84*signal + .16*np.roll(signal, 1385)))
    with wave.open(str(path), "wb") as out:
        out.setparams((2, 2, rate, 0, "NONE", "not compressed"))
        out.writeframes((stereo * t_fade[:, None] * 4600).astype("<i2").tobytes())

def make_filters():
    font = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    bold = "/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf"
    chain = ["scale=1920:1080:flags=lanczos", "fps=60", "setsar=1"]
    def box(x,y,w,h,color,start,end):
        chain.append(f"drawbox=x={x}:y={y}:w={w}:h={h}:color={color}:t=fill:enable='between(t,{start},{end})'")
    def line(text,x,y,size,start,end,heavy=False,color="white"):
        f = bold if heavy else font
        chain.append(f"drawtext=fontfile={f}:text='{text}':x={x}:y={y}:fontsize={size}:fontcolor={color}:"
                     f"enable='between(t,{start},{end})':shadowcolor=black@0.65:shadowx=2:shadowy=2")
    box(0,0,1920,1080,"black@0.25",0,4.1)
    box(98,770,94,4,"0xBFD6AD",.35,4.1)
    line("SAIGE",92,782,117,.35,4.1,True)
    line("SHOWCASE 02    /    COURTYARD",102,927,26,.35,4.1)
    for caption,start,end in [("IMPORTED PBR MATERIALS",7.25,10.9),("CAPSULE CHARACTER",13.4,17.5),
                              ("TRIANGLE COLLISION",39.4,43.3),("DYNAMIC SUNLIGHT",52.1,55.4)]:
        box(101,937,25,3,"0xBFD6AD",start,end)
        line(caption,141,919,24,start,end,True)
    box(0,0,1920,1080,"black@0.32",55.8,60)
    line("SAIGE","(w-text_w)/2",457,94,55.8,60,True)
    line("CURRENT DEVELOPMENT BUILD","(w-text_w)/2",570,23,55.8,60)
    chain.extend(["fade=t=in:st=0:d=0.65", "fade=t=out:st=58.7:d=1.3", "format=yuv420p"])
    return ",".join(chain)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--environment", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    ap.add_argument("--width", default=960, type=int)
    ap.add_argument("--height", default=540, type=int)
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="saige-capture-") as tmp:
        scratch = Path(tmp)
        fifo = scratch / "frames.ppm"
        os.mkfifo(fifo)
        audio = scratch / "score.wav"
        soundbed(audio)
        # Keep readers from hanging if the renderer fails before opening its writer.
        keeper = os.open(fifo, os.O_RDWR | os.O_NONBLOCK)
        out = args.out / "SAIGE-Showcase-02.mp4"
        encoder = subprocess.Popen(["ffmpeg","-nostdin","-y","-loglevel","warning","-filter_threads","1",
            "-f","image2pipe","-framerate","30","-vcodec","ppm","-i",str(fifo),"-i",str(audio),
            "-vf",make_filters(),"-c:v","libx264","-threads","2","-preset","veryfast","-crf","20",
            "-profile:v","high","-level:v","4.2","-maxrate","10M","-bufsize","20M","-color_primaries","bt709",
            "-color_trc","bt709","-colorspace","bt709","-c:a","aac","-b:a","128k","-movflags","+faststart",
            "-shortest",str(out)])
        capture = subprocess.Popen([args.exe,"--prop",str(args.environment/"wooden_crate_01.gltf"),"--out",str(args.out),
            "--frames-pipe",str(fifo),"--width",str(args.width),"--height",str(args.height),"--stills","0",
            "--capture-every","2","--sequence-seconds","60","--validate","--self-check"])
        try:
            while capture.poll() is None:
                if encoder.poll() is not None:
                    raise RuntimeError(f"Encoder stopped during capture ({encoder.returncode})")
                time.sleep(.5)
            if capture.returncode != 0:
                raise RuntimeError(f"SAIGE capture failed ({capture.returncode})")
        finally:
            os.close(keeper)
            if capture.poll() is None:
                capture.terminate()
                capture.wait()
        if encoder.wait() != 0:
            raise RuntimeError("H.264 encoding failed")
        print(out, flush=True)
if __name__=="__main__":
    main()
