#!/usr/bin/env python3
"""Makes a contact sheet (PNG) from the PPM frames saige_anim_viewer wrote.

  python3 tools/animation/viewer/sheet.py OUT_DIR [--columns 4] [--every 4] [--width 320] [--out sheet.png]

Needs Pillow (the tools venv from tools/production/setup.sh has it). The frames carry no picture of what stage each is from: report.json lists it per frame.
"""
import argparse, glob, json, os, sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.exit("Pillow is needed (pip install pillow, or the tools venv from tools/production/setup.sh)")

ap = argparse.ArgumentParser()
ap.add_argument("dir")
ap.add_argument("--columns", type=int, default=4)
ap.add_argument("--every", type=int, default=4)
ap.add_argument("--width", type=int, default=320)
ap.add_argument("--out", default=None)
args = ap.parse_args()
files = sorted(glob.glob(os.path.join(args.dir, "frame_*.ppm")))[:: args.every]
if not files:
    sys.exit("no frames in " + args.dir)
stage = {}
try:
    for f in json.load(open(os.path.join(args.dir, "report.json")))["frames"]:
        stage[f["file"]] = f"{f['time_s']:.1f}s {f['stage']}"
except (OSError, KeyError, ValueError):
    pass
first = Image.open(files[0]).convert("RGB")
w = args.width
h = round(first.height * w / first.width)
rows = (len(files) + args.columns - 1) // args.columns
sheet = Image.new("RGB", (w * args.columns, h * rows))
draw = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert("RGB").resize((w, h))
    x, y = (i % args.columns) * w, (i // args.columns) * h
    sheet.paste(im, (x, y))
    draw.text((x + 4, y + 3), stage.get(os.path.basename(f), os.path.basename(f)), fill=(255, 255, 255))
out = args.out or os.path.join(args.dir, "sheet.png")
sheet.save(out)
print(out, sheet.size)
