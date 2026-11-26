#!/usr/bin/env python3
"""Showcase 04 master sweep: decodes every frame of a video and looks for transient defects a person sees as flicker or popping.

Per frame t (luma, 480x272; frames t-1, t, t+1):
  pulse     fraction of pixels that jump away from t-1 and come back at t+1 (|f_t - mean(f_{t-1}, f_{t+1})| > T while |f_{t-1} - f_{t+1}| < T/3): a one-frame pulse is what flicker and
            depth-buffer fighting look like; camera motion produces monotone changes, not pulses.
  pop       the number of 16x16 blocks that change a lot between t-1 and t (> POP_ABS and > 6x the frame's median block change) while the rest of the frame barely moves: something appeared,
            disappeared or switched, locally. A cut or a fast camera move changes every block and is not counted.
  zblocks   the number of 16x16 blocks in which more than 25% of the pixels pulse: a patch of surface fighting for the depth buffer (structural), as opposed to thin features shimmering (temporal
            aliasing: lane markings, window frames, fences), which pulse in scattered pixels
  motion    the median block change (how much the camera is moving)
Events are runs of consecutive frames over a threshold. Output: a text report (--out) and, with --frames-dir, the PNG of the worst frame of each event at full size.

Usage: frame_sweep.py VIDEO [--out report.txt] [--start S --end E (seconds)] [--frames-dir DIR]
"""
import argparse
import subprocess
import sys

import numpy as np

W, H = 480, 272
BLOCK = 16
T = 24.0          # luma levels
POP_ABS = 40.0    # mean luma change in a block


def frames(video, start, end):
    cmd = ["ffmpeg", "-loglevel", "error", "-ss", str(start)]
    if end is not None:
        cmd += ["-t", str(end - start)]
    cmd += ["-i", video, "-vf", "scale=%d:%d,format=gray" % (W, H), "-f", "rawvideo", "-pix_fmt", "gray", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    size = W * H
    while True:
        buf = p.stdout.read(size)
        if len(buf) < size:
            break
        yield np.frombuffer(buf, dtype=np.uint8).reshape(H, W).astype(np.float32)
    p.wait()


def block_mean(a):
    return a.reshape(H // BLOCK, BLOCK, W // BLOCK, BLOCK).mean(axis=(1, 3))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("video")
    ap.add_argument("--out", default=None)
    ap.add_argument("--start", type=float, default=0.0)
    ap.add_argument("--end", type=float, default=None)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--frames-dir", default=None)
    ap.add_argument("--pulse-threshold", type=float, default=0.004, help="fraction of pixels")
    ap.add_argument("--pop-blocks", type=int, default=3)
    ap.add_argument("--zblocks", type=int, default=2, help="a frame has structural flicker when at least this many blocks pulse")
    ap.add_argument("--assert-no-structural-flicker", action="store_true", help="exit 1 when any frame has structural flicker")
    args = ap.parse_args()

    rows = []   # (frame index, pulse, pop blocks, motion, max block diff)
    prev = cur = None
    idx0 = int(round(args.start * args.fps))
    i = idx0
    pending = []
    for nxt in frames(args.video, args.start, args.end):
        if prev is not None and cur is not None:
            pulse_mask = (np.abs(cur - 0.5 * (prev + nxt)) > T) & (np.abs(prev - nxt) < T / 3.0)
            pulse = float(pulse_mask.mean())
            zb = int((block_mean(pulse_mask.astype(np.float32)) > 0.25).sum())
            d = np.abs(block_mean(cur) - block_mean(prev))
            med = float(np.median(d))
            pop = int(((d > POP_ABS) & (d > 6.0 * med + 10.0)).sum())
            rows.append((i - 1, pulse, pop, med, float(d.max()), zb))
        prev, cur = cur, nxt
        i += 1
    # events
    def events(key, test):
        out = []
        run = []
        for r in rows:
            if test(r):
                run.append(r)
            elif run:
                out.append(run)
                run = []
        if run:
            out.append(run)
        return out

    lines = []
    lines.append("sweep of %s: %d frames (%d to %d)" % (args.video, len(rows), rows[0][0] if rows else 0, rows[-1][0] if rows else 0))
    pe = events("pulse", lambda r: r[1] > args.pulse_threshold)
    ze = events("zblocks", lambda r: r[5] >= args.zblocks)
    po = events("pop", lambda r: r[2] >= args.pop_blocks)
    lines.append("")
    lines.append("STRUCTURAL flicker events (>= %d blocks in which more than 25%% of the pixels pulse): %d" % (args.zblocks, len(ze)))
    for run in ze:
        w = max(run, key=lambda r: r[5])
        lines.append("  %7.2f s - %7.2f s  frames %d-%d  worst frame %d: %d blocks (pulse %.2f%%)" % (run[0][0] / args.fps, run[-1][0] / args.fps, run[0][0], run[-1][0], w[0], w[5], w[1] * 100))
    lines.append("")
    lines.append("flicker events (one-frame pulses on more than %.2f%% of pixels): %d" % (args.pulse_threshold * 100, len(pe)))
    for run in pe:
        w = max(run, key=lambda r: r[1])
        lines.append("  %7.2f s - %7.2f s  frames %d-%d  worst frame %d: %.2f%% pulse pixels (motion %.1f)" % (run[0][0] / args.fps, run[-1][0] / args.fps, run[0][0], run[-1][0], w[0], w[1] * 100, w[3]))
    lines.append("")
    lines.append("popping events (>= %d blocks change locally while the frame is otherwise steady): %d" % (args.pop_blocks, len(po)))
    for run in po:
        w = max(run, key=lambda r: r[2])
        lines.append("  %7.2f s - %7.2f s  frames %d-%d  worst frame %d: %d blocks (max block change %.0f, median %.1f)" % (run[0][0] / args.fps, run[-1][0] / args.fps, run[0][0], run[-1][0], w[0], w[2], w[4], w[3]))
    text = "\n".join(lines)
    print(text)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text + "\n")
        np.save(args.out + ".npy", np.array(rows))
    if args.frames_dir:
        import os
        os.makedirs(args.frames_dir, exist_ok=True)
        for tag, evs, key in (("structural", ze, 5), ("pop", po, 2)):
            for run in evs[:40]:
                w = max(run, key=lambda r: r[key])
                subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-ss", "%.4f" % (w[0] / args.fps), "-i", args.video, "-frames:v", "1", os.path.join(args.frames_dir, "%s_%05d.png" % (tag, w[0]))], check=False)
    if args.assert_no_structural_flicker and ze:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
