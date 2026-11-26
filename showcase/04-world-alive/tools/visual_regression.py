#!/usr/bin/env python3
"""Showcase 04 visual regression. Numeric tests (alive_*_test) pass or fail on data; these render the real film frames through the real renderer and check what the picture shows.

Each case renders a frame of the film twice, with one controlled difference, and asserts a property a correct renderer must have. No golden images are stored (they would depend on the licensed
texture packs): the reference is always the same frame rendered by the same binary.

  interior_never_changes_exterior   frame 1245 (the cafe's side wall, grazing sun): the exterior must look the same with the building interiors left out. A second surface in the wall's plane
                                    (the depth-buffer flicker of the film at 0:41-0:42) changes it.
  walls_complete                    frames 690, 990 (a corner with a deeper building next to a shallower one): drawing every face whole must not change the picture. A wall the renderer
                                    dropped (the film at 0:23-0:34) changes it.
  scenery_does_not_cover_the_road   frames 1110, 1170 (a car driving south on an avenue, 0:37-0:39): the lower half of the picture (the district's own road and pavement) must be identical
                                    with and without the scenery copies. A copy's
                                    raised promenade across the road (the film at 0:37-0:40) changes it.
  camera_clear_of_foliage           the whole film's camera path: no frame has the eye inside leaves or its first 6 m of view through them (the film at 1:30-1:33).

Run with --mutants to prove the cases can fail: each case is repeated with the film's behaviour switched back on (SAIGE_ALIVE_LEGACY_*), and must then be reported as failing.
Exit status: 0 all pass, 1 a case failed (or, with --mutants, a mutant was NOT caught), 77 skipped (no renderer, no GPU or no texture packs).
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

FILM = ["--film", "--seed", "8", "--film-seconds", "180", "--preroll", "25", "--fps", "30", "--time-scale", "47"]


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # P6 width height maxval, whitespace separated, then raw bytes.
    parts = data.split(maxsplit=4)
    assert parts[0] == b"P6"
    w, h = int(parts[1]), int(parts[2])
    header = len(parts[0]) + len(parts[1]) + len(parts[2]) + len(parts[3]) + 4
    px = data[header:header + w * h * 3]
    return w, h, px


def run_frames(app, assets, scout, frames, env_extra, outdir, width, height):
    env = dict(os.environ)
    env.update(env_extra)
    os.makedirs(outdir, exist_ok=True)
    cmd = [app, *FILM, "--scout-file", scout, "--assets", assets, "--width", str(width), "--height", str(height), "--shadow-map", "2048",
           "--frames", ",".join(str(f) for f in frames), "--out", outdir]
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError("renderer failed (%d): %s" % (r.returncode, r.stdout[-400:]))
    return {f: read_ppm(os.path.join(outdir, "frame_%05d.ppm" % f)) for f in frames}


def region_diff(a, b, x0, y0, x1, y1):
    """Fraction of pixels in the region whose channels differ by more than 3/255, and the largest difference."""
    w, h, pa = a
    _, _, pb = b
    bad = 0
    n = 0
    worst = 0
    for y in range(y0, y1):
        row = y * w * 3
        for x in range(x0, x1):
            i = row + x * 3
            d = max(abs(pa[i] - pb[i]), abs(pa[i + 1] - pb[i + 1]), abs(pa[i + 2] - pb[i + 2]))
            n += 1
            if d > 3:
                bad += 1
            worst = max(worst, d)
    return bad / max(1, n), worst


def case(name, frames, regions, ref_env, test_env, args, work, width, height):
    ref = run_frames(args.app, args.assets, args.scout, frames, ref_env, os.path.join(work, name + "_ref"), width, height)
    out = run_frames(args.app, args.assets, args.scout, frames, test_env, os.path.join(work, name + "_test"), width, height)
    worst_frac = 0.0
    for f in frames:
        x0, y0, x1, y1 = regions[f]
        frac, worst = region_diff(ref[f], out[f], int(x0 * width), int(y0 * height), int(x1 * width), int(y1 * height))
        print("    %s frame %d: %.3f%% of the region differs (max channel difference %d)" % (name, f, frac * 100.0, worst))
        worst_frac = max(worst_frac, frac)
    return worst_frac


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--assets", default="/tmp/saige-cache")
    ap.add_argument("--scout", default=None, help="scout report of seed 8 (default: generated with --scout)")
    ap.add_argument("--scout-tool", default=None, help="path of alive_scout, to generate the scout report")
    ap.add_argument("--mutants", action="store_true")
    ap.add_argument("--keep", default=None)
    ap.add_argument("--only", default=None, help="run only the named case (a case name, or camera_clear_of_foliage)")
    ap.add_argument("--tolerance", type=float, default=0.002, help="largest fraction of differing pixels a passing case may have")
    args = ap.parse_args()

    if not os.path.exists(args.app) or not os.path.isdir(os.path.join(args.assets, "polyhaven")):
        print("skipped: renderer or the Poly Haven texture cache is missing")
        return 77
    work = args.keep or tempfile.mkdtemp(prefix="alive_vr_")
    try:
        if args.scout is None:
            if args.scout_tool is None:
                print("skipped: no scout report")
                return 77
            args.scout = os.path.join(work, "scout8.txt")
            with open(args.scout, "w") as f:
                subprocess.run([args.scout_tool, "8", "230"], stdout=f, check=True)
        W, H = 960, 540
        failures = 0
        cases = [
            # name, frames, region per frame (fractions x0,y0,x1,y1), reference env, test env, mutant env (the film's behaviour)
            ("interior_never_changes_exterior", [1245], {1245: (0.0, 0.0, 0.3, 0.55)}, {"SAIGE_ALIVE_SKIP": "interior"}, {}, {"SAIGE_ALIVE_LEGACY_INTERIOR_WALLS": "1"}),
            ("walls_complete", [690, 990], {690: (0.0, 0.0, 1.0, 0.8), 990: (0.0, 0.0, 1.0, 0.8)}, {"SAIGE_ALIVE_ALLFACES": "1"}, {}, {"SAIGE_ALIVE_LEGACY_HIDDEN": "1"}),
            ("scenery_does_not_cover_the_road", [1110, 1170], {1110: (0.0, 0.58, 1.0, 1.0), 1170: (0.0, 0.58, 1.0, 1.0)}, {"SAIGE_ALIVE_NOCOPIES": "1"}, {}, {"SAIGE_ALIVE_LEGACY_SCENERY": "1"}),
        ]
        for name, frames, regions, ref_env, test_env, mutant_env in cases:
            if args.only is not None and args.only != name:
                continue
            print("case %s" % name)
            frac = case(name, frames, regions, ref_env, test_env, args, work, W, H)
            ok = frac <= args.tolerance
            print("  %s (%.3f%% differing, tolerance %.3f%%)" % ("PASS" if ok else "FAIL", frac * 100.0, args.tolerance * 100.0))
            failures += 0 if ok else 1
            if args.mutants:
                # The mutant is the same comparison with the film's behaviour switched on in the TEST render (the reference render is unchanged).
                menv = dict(test_env)
                menv.update(mutant_env)
                mfrac = case(name + "_mutant", frames, regions, ref_env, menv, args, work, W, H)
                caught = mfrac > args.tolerance
                print("  mutant (%s): %s (%.3f%% differing)" % (",".join(mutant_env), "CAUGHT" if caught else "NOT CAUGHT", mfrac * 100.0))
                failures += 0 if caught else 1
        # The camera path needs no renderer pass.
        if args.only is not None and args.only != "camera_clear_of_foliage":
            print("visual regression: %s" % ("ok" if failures == 0 else "%d failure(s)" % failures))
            return 0 if failures == 0 else 1
        print("case camera_clear_of_foliage")
        r = subprocess.run([args.app, *FILM, "--scout-file", args.scout, "--assets", args.assets, "--width", "320", "--height", "180", "--no-render", "--check-camera"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        last = [l for l in r.stdout.splitlines() if l.startswith("camera check")]
        print("    " + (last[-1] if last else r.stdout[-200:]))
        ok = r.returncode == 0
        print("  %s" % ("PASS" if ok else "FAIL"))
        failures += 0 if ok else 1
        if args.mutants:
            env = dict(os.environ)
            env["SAIGE_ALIVE_NO_CANOPY_CLEAR"] = "1"
            r2 = subprocess.run([args.app, *FILM, "--scout-file", args.scout, "--assets", args.assets, "--width", "320", "--height", "180", "--no-render", "--check-camera"], env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            last = [l for l in r2.stdout.splitlines() if l.startswith("camera check")]
            print("  mutant (SAIGE_ALIVE_NO_CANOPY_CLEAR): %s -- %s" % ("CAUGHT" if r2.returncode == 3 else "NOT CAUGHT", last[-1] if last else ""))
            failures += 0 if r2.returncode == 3 else 1
        print("visual regression: %s" % ("ok" if failures == 0 else "%d failure(s)" % failures))
        return 0 if failures == 0 else 1
    finally:
        if args.keep is None:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
