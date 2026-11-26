#!/usr/bin/env python3
"""Compose the validation evidence from `ssr_visual_dump` output: labelled comparison sheets (stills) and the MP4 (clip frames).

  compose_ssr_evidence.py <stills_dir> <clip_dir> <out_dir>

For every scene a full-frame sheet (ground truth, bilinear, bicubic, TAA-U, SSR on the CPU, SSR on the GPU, each scaled to 640x360) and a 1:1 detail
sheet (a 240x135 crop at the place where the SSR(GPU) error against the truth is largest, enlarged 3x with nearest neighbour). Labels are drawn on
the images; nothing is retouched."""
import os, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw

METHODS = [("gt", "ground truth (4x4 supersampled)"), ("bilinear", "bilinear"), ("bicubic", "bicubic"), ("taau", "TAA-U baseline (delivered)"),
           ("ssr_cpu", "SSR CPU reference"), ("ssr_gpu", "SSR Vulkan compute (llvmpipe)")]

def label(img, text):
    d = ImageDraw.Draw(img); d.rectangle([0, 0, img.width, 16], fill=(0, 0, 0)); d.text((4, 2), text, fill=(255, 255, 255)); return img

def load(path): return Image.open(path).convert("RGB")

def main():
    stills, clip, out = sys.argv[1:4]
    os.makedirs(out, exist_ok=True)
    scenes = sorted({f.rsplit("_gt.ppm", 1)[0] for f in os.listdir(stills) if f.endswith("_gt.ppm")})
    for sc in scenes:
        imgs = {m: load(os.path.join(stills, "%s_%s.ppm" % (sc, m))) for m, _ in METHODS}
        # full-frame sheet: 3 x 2 panels of 640x360
        sheet = Image.new("RGB", (1920, 720))
        for i, (m, name) in enumerate(METHODS):
            p = imgs[m].resize((640, 360), Image.LANCZOS); label(p, name)
            sheet.paste(p, ((i % 3) * 640, (i // 3) * 360))
        sheet.save(os.path.join(out, "%s_sheet_1080p_downscaled.jpg" % sc), quality=88)
        # detail: the 240x135 window where |ssr_gpu - gt| is largest (box-filtered over 60x34 blocks)
        a = np.asarray(imgs["ssr_gpu"], dtype=np.float32); g = np.asarray(imgs["gt"], dtype=np.float32)
        e = np.abs(a - g).mean(axis=2)
        bh, bw = 135, 240
        best, bx, by = -1.0, 0, 0
        for y in range(0, e.shape[0] - bh, 45):
            for x in range(0, e.shape[1] - bw, 80):
                v = float(e[y:y + bh, x:x + bw].mean())
                if v > best: best, bx, by = v, x, y
        det = Image.new("RGB", (1440, 810))
        for i, (m, name) in enumerate([("gt", "ground truth"), ("bilinear", "bilinear"), ("taau", "TAA-U baseline"), ("ssr_gpu", "SSR Vulkan compute (1:1 crop x3)")]):
            c = imgs[m].crop((bx, by, bx + bw, by + bh)).resize((720, 405), Image.NEAREST); label(c, "%s  [crop at %d,%d of 1920x1080]" % (name, bx, by))
            det.paste(c, ((i % 2) * 720, (i // 2) * 405))
        det.save(os.path.join(out, "%s_detail_1to1_crop.jpg" % sc), quality=90)
        print(sc, "detail crop at", bx, by, "mean abs err (8-bit) %.2f" % best)
    # clip
    if os.path.isdir(clip):
        frames = sorted(f for f in os.listdir(clip) if f.endswith(".ppm"))
        if frames:
            tmp = os.path.join(clip, "png"); os.makedirs(tmp, exist_ok=True)
            for i, f in enumerate(frames):
                im = load(os.path.join(clip, f)); d = ImageDraw.Draw(im)
                d.rectangle([0, 0, 1280, 14], fill=(0, 0, 0)); d.rectangle([0, 360, 1280, 374], fill=(0, 0, 0))
                d.text((4, 1), "ground truth (1:1 centre crop, 960x540 output)", fill=(255, 255, 255)); d.text((644, 1), "TAA-U baseline", fill=(255, 255, 255))
                d.text((4, 361), "SSR Vulkan compute (llvmpipe)  frame %d" % i, fill=(255, 255, 255)); d.text((644, 361), "|SSR - truth| x6", fill=(255, 255, 255))
                im.save(os.path.join(tmp, "f%04d.png" % i))
            subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30", "-i", os.path.join(tmp, "f%04d.png"), "-c:v", "libx264", "-crf", "20", "-pix_fmt", "yuv420p",
                            "-movflags", "+faststart", os.path.join(out, "ssr_gpu_720p30_diagnostic.mp4")], check=True)
            # contact sheet of 6 frames for review without playback
            pick = [0, 20, 40, 74, 76, 140]
            cs = Image.new("RGB", (1920, 720))
            for i, k in enumerate(pick):
                k = min(k, len(frames) - 1)
                cs.paste(Image.open(os.path.join(tmp, "f%04d.png" % k)).resize((640, 360), Image.LANCZOS), ((i % 3) * 640, (i // 3) * 360))
            cs.save(os.path.join(out, "ssr_gpu_clip_contact_sheet.jpg"), quality=88)
            print("clip:", len(frames), "frames")
main()
