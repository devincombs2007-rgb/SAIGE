#!/usr/bin/env python3
"""Shadow stability test. Two controlled sequences of consecutive frames, each swept with the same pulse metric as the master sweep (frame_sweep.py): a pixel that jumps and returns within a
frame is flicker, and a shadow edge that moves steadily is not.

  camera   the camera dollies 0.27 m per frame (8 m/s at 30 fps) along an avenue, the sun fixed and low (long shadows): cascade slices slide over the world, texel snapping and
           cascade blending are what keep the shadow edges still.
  sun      the camera is fixed and the sun advances at the film's time of day rate (time-scale 47: 0.000435 h per frame): the light's texel grid rotates a little each frame.

Each sequence is rendered with shadows and with shadows off; the metric is applied to the difference of the two (the shadows' own contribution), so other shimmer (thin lane lines) cancels. With --mutants the
sequences are also rendered with cascade texel snapping off, reported for information.

What it can and cannot see (measured, see docs/showcase/04-defect-register.md S4-009): at the film's settings texel snapping removes shimmer smaller than a pixel, so switching it off changes the
pulse by 0.0006 percentage points here: the --mutants run REPORTS whether snapping is visible and does not fail on it. What the test does guard is the level of pulse the shadows add (thin casters
such as poles alias without temporal anti-aliasing): a new shadow artefact that pulses over more of the picture than that baseline fails it.

Usage: shadow_stability.py --app saige_alive [--frames 24] [--keep DIR] [--max-added-pulse 0.006]
Exit status: 0 stable, 1 the shadows add more pulse than allowed, 77 skipped (no renderer or textures).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np

W, H = 960, 540


def run(cmd, env=None):
    e = dict(os.environ)
    if env:
        e.update(env)
    r = subprocess.run(cmd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError("failed (%d): %s\n%s" % (r.returncode, " ".join(cmd[:4]), r.stdout[-300:]))
    return r.stdout


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(maxsplit=4)
    w, h = int(parts[1]), int(parts[2])
    header = len(parts[0]) + len(parts[1]) + len(parts[2]) + len(parts[3]) + 4
    return np.frombuffer(data[header:header + w * h * 3], dtype=np.uint8).reshape(h, w, 3)


def luma(a):
    return a.astype(np.float32) @ np.array([0.299, 0.587, 0.114], dtype=np.float32)


def pulse_stats(lumas):
    """Mean pulse fraction and the largest number of 16x16 blocks with more than 25% pulse pixels over a sequence of luma images (same definitions as frame_sweep.py)."""
    pulses, zmax = [], 0
    for i in range(1, len(lumas) - 1):
        a, b, c = lumas[i - 1], lumas[i], lumas[i + 1]
        m = (np.abs(b - 0.5 * (a + c)) > 24.0) & (np.abs(a - c) < 8.0)
        pulses.append(float(m.mean()))
        hh, ww = (H // 16) * 16, (W // 16) * 16
        blocks = m[:hh, :ww].reshape(hh // 16, 16, ww // 16, 16).mean(axis=(1, 3))
        zmax = max(zmax, int((blocks > 0.25).sum()))
    return float(np.mean(pulses)), zmax


def shadow_signal(on, off):
    """The shadows' own contribution: luma with shadows minus luma without. Whatever else pulses (thin lane lines moving across pixels) is in both and cancels."""
    return [luma(a) - luma(b) for a, b in zip(on, off)]


def render_camera_path(app, assets, frames, extra, outdir):
    os.makedirs(outdir, exist_ok=True)
    cmd = [app, "--assets", assets, "--width", str(W), "--height", str(H), "--shadow-map", "2048", "--hour", "17.5", "--out", outdir]
    cmd += extra
    for k in range(frames):
        y = -30.0 - 0.27 * k
        cmd += ["--still", "3.5,%.3f,2.2,3.0,%.3f,1.4,55" % (y, y - 20.0)]
    run(cmd)
    return [read_ppm(os.path.join(outdir, "still_%02d.ppm" % k)) for k in range(frames)]


def render_sun_path(app, assets, frames, extra, outdir):
    os.makedirs(outdir, exist_ok=True)
    cmd = [app, "--assets", assets, "--width", str(W), "--height", str(H), "--shadow-map", "2048", "--hour", "17.5", "--hour-step", "0.000435", "--out", outdir]
    cmd += extra
    for _ in range(frames):
        cmd += ["--still", "3.5,-30,2.2,3.0,-50,1.4,55"]
    run(cmd)
    return [read_ppm(os.path.join(outdir, "still_%02d.ppm" % k)) for k in range(frames)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--assets", default="/tmp/saige-cache")
    ap.add_argument("--frames", type=int, default=24)
    ap.add_argument("--keep", default=None)
    ap.add_argument("--mutants", action="store_true", help="also run with cascade texel snapping off, which must be caught")
    ap.add_argument("--max-added-pulse", type=float, default=0.006, help="largest mean pulse fraction the shadows may add (the measured aliasing floor is 0.0038)")
    args = ap.parse_args()
    if not os.path.exists(args.app) or not os.path.isdir(os.path.join(args.assets, "polyhaven")):
        print("skipped: renderer or the Poly Haven texture cache is missing")
        return 77
    work = args.keep or tempfile.mkdtemp(prefix="alive_shadow_")
    failures = 0
    try:
        for name, fn in (("camera", render_camera_path), ("sun", render_sun_path)):
            on = fn(args.app, args.assets, args.frames, [], os.path.join(work, name + "_on"))
            off = fn(args.app, args.assets, args.frames, ["--disable", "shadows"], os.path.join(work, name + "_off"))
            p_sig, z_sig = pulse_stats(shadow_signal(on, off))
            ok = p_sig <= args.max_added_pulse
            print("%-7s shadow signal (shadows on minus off): pulse %.4f%% (max %d blocks)   %s" % (name, p_sig * 100, z_sig, "ok" if ok else "FAIL"))
            failures += 0 if ok else 1
            if args.mutants:
                mut = fn(args.app, args.assets, args.frames, ["--disable", "stabilize"], os.path.join(work, name + "_mut"))
                pm, zm = pulse_stats(shadow_signal(mut, off))
                print("%-7s cascades not snapped to texels (informational): pulse %.4f%% (max %d blocks), %+.4f points against snapped" % (name, pm * 100, zm, (pm - p_sig) * 100))
        print("shadow stability: %s" % ("ok" if failures == 0 else "%d failure(s)" % failures))
        return 0 if failures == 0 else 1
    finally:
        if args.keep is None:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
