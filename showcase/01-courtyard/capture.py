#!/usr/bin/env python3
"""Runs saige_showcase and turns its PPM frames into the viewable artifacts: PNG stills, a contact sheet, an H.264 video, a GIF and
a top-down plot of the character's path against the collision boxes.

    capture.py --exe build/showcase/saige_showcase --out captures [--still-size 1920x1080] [--video-size 1280x720] [--ssaa 2]

Needs Pillow and ffmpeg. Everything it writes is derived from frames the renderer produced; nothing is drawn by this script except
the path plot, which is labeled as a plot.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))


def run(cmd):
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True)


def size(text):
    w, h = text.lower().split("x")
    return int(w), int(h)


def convert_stills(src, dst):
    os.makedirs(dst, exist_ok=True)
    names = []
    for f in sorted(os.listdir(src)):
        if f.endswith(".ppm"):
            Image.open(os.path.join(src, f)).save(os.path.join(dst, f[:-4] + ".png"), optimize=True)
            names.append(f[:-4])
    return names


def contact_sheet(png_dir, names, out_path, columns=2, cell_width=960):
    images = [Image.open(os.path.join(png_dir, n + ".png")).convert("RGB") for n in names]
    cell_height = round(cell_width * images[0].height / images[0].width)
    rows = (len(images) + columns - 1) // columns
    sheet = Image.new("RGB", (columns * cell_width, rows * cell_height), (20, 20, 24))
    for i, im in enumerate(images):
        sheet.paste(im.resize((cell_width, cell_height), Image.LANCZOS), ((i % columns) * cell_width, (i // columns) * cell_height))
    sheet.save(out_path, optimize=True)


def path_plot(scene_json, trace_json, out_path, px_per_m=34):
    scene = json.load(open(scene_json))
    trace = json.load(open(trace_json))
    xs = [c["min"][0] for c in scene["colliders"] if c["name"] != "ground"] + [c["max"][0] for c in scene["colliders"] if c["name"] != "ground"]
    ys = [c["min"][1] for c in scene["colliders"] if c["name"] != "ground"] + [c["max"][1] for c in scene["colliders"] if c["name"] != "ground"]
    x0, x1, y0, y1 = min(xs) - 2, max(xs) + 2, min(ys) - 2, max(ys) + 2
    W, H = int((x1 - x0) * px_per_m), int((y1 - y0) * px_per_m)

    def P(x, y):  # +X to the right, +Y up
        return ((x - x0) * px_per_m, (y1 - y) * px_per_m)

    im = Image.new("RGB", (W, H + 70), (24, 26, 32))
    d = ImageDraw.Draw(im)
    font = ImageFont.load_default()
    for c in scene["colliders"]:
        if c["name"] == "ground":
            continue
        (ax, ay), (bx, by) = P(c["min"][0], c["max"][1]), P(c["max"][0], c["min"][1])
        top = c["max"][2]
        shade = int(70 + min(top, 4.0) / 4.0 * 110)
        d.rectangle([ax, ay, bx, by], fill=(shade, shade, shade + 6), outline=(150, 150, 160))
    for t in scene.get("triangles", []):  # ramps and the chute: drawn as their top-down projection, shaded by mean height, outlined in teal
        pts = [P(t[k][0], t[k][1]) for k in ("a", "b", "c")]
        mean_z = (t["a"][2] + t["b"][2] + t["c"][2]) / 3.0
        shade = int(70 + min(mean_z, 4.0) / 4.0 * 110)
        d.polygon(pts, fill=(shade, shade + 14, shade + 10), outline=(90, 200, 190))
    cols = trace["columns"]
    ix, iy, iz, ig = cols.index("x"), cols.index("y"), cols.index("z"), cols.index("grounded")
    pts = trace["ticks"]
    for a, b in zip(pts, pts[1:]):
        height = max(a[iz], b[iz])
        color = (255, 170, 60) if a[ig] and b[ig] and height < 0.05 else (80, 200, 255) if a[ig] and b[ig] else (255, 90, 90)
        d.line([P(a[ix], a[iy]), P(b[ix], b[iy])], fill=color, width=3)
    palette = {"jump": (255, 80, 80), "land": (90, 230, 120), "step_up": (90, 140, 255), "waypoint_timed_out": (255, 230, 80), "blocked": (255, 255, 255)}
    for e in trace["events"]:
        if e["kind"] in palette:
            p = pts[e["tick"]]
            x, y = P(p[ix], p[iy])
            d.ellipse([x - 5, y - 5, x + 5, y + 5], outline=palette[e["kind"]], width=2)
    sx, sy = P(pts[0][ix], pts[0][iy])
    d.text((sx + 8, sy - 14), "start", fill=(255, 255, 255), font=font)
    ex, ey = P(pts[-1][ix], pts[-1][iy])
    d.text((ex + 8, ey - 14), "end", fill=(255, 255, 255), font=font)
    legend = [("walking on the ground", (255, 170, 60)), ("walking on a raised surface", (80, 200, 255)), ("airborne", (255, 90, 90)),
              ("jump", palette["jump"]), ("landing", palette["land"]), ("step up", palette["step_up"]), ("slide along the wall", palette["waypoint_timed_out"]), ("ramp / chute triangles", (90, 200, 190))]
    x = 12
    for text, color in legend:
        d.rectangle([x, H + 14, x + 14, H + 28], fill=color)
        d.text((x + 20, H + 16), text, fill=(235, 235, 240), font=font)
        x += 20 + int(font.getlength(text)) + 22
    d.text((12, H + 42), f"Showcase 01 course: top view of the scripted CapsuleCharacter run ({len(pts)} fixed ticks at 60 Hz, collision boxes shaded by height). A plot of the "
           "simulation trace, not a render.", fill=(180, 184, 196), font=font)
    im.save(out_path, optimize=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--assets", default=os.path.join(HERE, "assets"))
    ap.add_argument("--still-size", default="1920x1080")
    ap.add_argument("--video-size", default="1280x720")
    ap.add_argument("--ssaa", type=int, default=2)
    ap.add_argument("--keep-frames", action="store_true")
    ap.add_argument("--validate", action="store_true")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    work = os.path.join(out, "_work")
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    flags = ["--validate"] if args.validate else []
    sw, sh = size(args.still_size)
    vw, vh = size(args.video_size)

    run([args.exe, "--assets", args.assets, "--out", os.path.join(work, "stills"), "--width", str(sw), "--height", str(sh), "--ssaa", str(args.ssaa),
         "--sequence-seconds", "0", "--self-check"] + flags)
    run([args.exe, "--assets", args.assets, "--out", os.path.join(work, "video"), "--width", str(vw), "--height", str(vh), "--ssaa", str(args.ssaa),
         "--stills", "0", "--self-check"] + flags)

    names = convert_stills(os.path.join(work, "stills", "stills"), os.path.join(out, "stills"))
    contact_sheet(os.path.join(out, "stills"), names, os.path.join(out, "contact-sheet.png"))

    seq = os.path.join(work, "video", "seq")
    run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30", "-i", os.path.join(seq, "frame_%05d.ppm"), "-c:v", "libx264", "-pix_fmt", "yuv420p",
         "-crf", "24", "-preset", "slow", "-movflags", "+faststart", os.path.join(out, "showcase-01-run.mp4")])
    palette = os.path.join(work, "palette.png")
    run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30", "-t", "16", "-i", os.path.join(seq, "frame_%05d.ppm"), "-vf", "fps=10,scale=480:-1:flags=lanczos,palettegen=max_colors=128", palette])
    run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30", "-t", "16", "-i", os.path.join(seq, "frame_%05d.ppm"), "-i", palette, "-lavfi",
         "fps=10,scale=480:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=bayer:bayer_scale=3", os.path.join(out, "showcase-01-first-16s.gif")])
    # A poster frame from the middle of the run, for the README.
    frames = sorted(f for f in os.listdir(seq) if f.endswith(".ppm"))
    Image.open(os.path.join(seq, frames[len(frames) // 2])).save(os.path.join(out, "poster.png"), optimize=True)

    for sub in ("stills", "video"):
        shutil.copy(os.path.join(work, sub, "report.json"), os.path.join(out, f"report-{sub}.json"))
        shutil.copy(os.path.join(work, sub, "perf.json"), os.path.join(out, f"perf-{sub}.json"))
    shutil.copy(os.path.join(work, "video", "trace.json"), os.path.join(out, "trace.json"))
    path_plot(os.path.join(args.assets, "scene.json"), os.path.join(out, "trace.json"), os.path.join(out, "course-path.png"))
    if not args.keep_frames:
        shutil.rmtree(work)
    print("captures written to", out)


if __name__ == "__main__":
    sys.exit(main())
