#!/usr/bin/env python3
"""Showcase 04 visual regression. Numeric tests (alive_*_test) pass or fail on data; these render the real film frames through the real renderer and check what the picture shows.

Each case renders a frame of the film twice, with one controlled difference, and asserts a property a correct renderer must have. No golden images are stored (they would depend on the licensed
texture packs): the reference is always the same frame rendered by the same binary.

  interior_never_changes_exterior   a fixed camera at the film's frame-1245 view (the cafe's side wall, a 9 degree sun): the exterior must look the same with the building interiors left out. A second surface in the wall's plane
                                    (the depth-buffer flicker of the film at 0:41-0:42) changes it.
  walls_complete                    the film cameras of frames 690, 990 as fixed stills (a corner with a deeper building next to a shallower one): drawing every face whole must not change the picture. A wall the renderer
                                    dropped (the film at 0:23-0:34) changes it.
  scenery_does_not_cover_the_road   the film cameras of frames 1110, 1170 as fixed stills (an avenue, 0:37-0:39): the lower half of the picture (the district's own road and pavement) must be identical
                                    with and without the scenery copies. A copy's
                                    raised promenade across the road (the film at 0:37-0:40) changes it.
  temporal_stability                41 stills along a straight dolly between the film's cameras of frames 1221 and 1261 (0:40.7, the cafe's side wall at a grazing sun, where the film flickered; needs ffmpeg): the frame sweep (frame_sweep.py) must find no
                                    structural flicker. The film's behaviour fails it (6 to 15 events of up to 23 blocks in the three ranges it was measured on).
  camera_clear_of_foliage           the whole film's camera path: no frame has the eye inside leaves or its first 6 m of view through them (the film at 1:30-1:33).

  scout_producers_agree             the scout file saige_alive writes for seed 8 and the one alive_scout writes are byte-identical (the two used to disagree, and the film's director read the wrong one as an accident at t = 0).

Run with --mutants to prove the cases can fail: each case is repeated with the film's behaviour switched back on (SAIGE_ALIVE_LEGACY_*), and must then be reported as failing.
--fixtures runs the same cases on generated stand-ins (tools/make_fixtures.py: coloured checker textures, flat normal and ARM maps; the app's procedural car; no residents) so that it needs nothing downloaded and can be a
required check in hosted CI. What that covers and what it does not is in the defect register's infrastructure section: geometry, depth, shadow and director paths yes; the third-party textures' detail, the car model, the props
and the residents no (the full-asset run, without --fixtures, keeps those). A run fails when: the case named by --only does not exist, no test executed, a required run is asked without --mutants, or (with --mutants) any intended mutant is not caught.
Every SAIGE_ALIVE_* variable inherited from the caller's environment is removed before any render (a baseline must not run with a mutation switched on) and each removal is reported.
Exit status: 0 all pass, 1 a case failed (or, with --mutants, a mutant was NOT caught), 77 skipped (no renderer or no texture cache). With SAIGE_REQUIRE_VISUAL=1 in the environment (hosted CI sets it
after restoring the assets with restore_assets.py) a missing renderer or cache is a FAILURE (exit 1), never a skip: a test that cannot run must not look like a test that passed.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

ANIM = None
FIXTURES = False
KNOWN_CASES = ["interior_never_changes_exterior", "walls_complete", "scenery_does_not_cover_the_road", "temporal_stability", "camera_clear_of_foliage", "scout_producers_agree"]
EXECUTED = 0   # renders and checks that really ran (a run that executed nothing is a failure)
FILM = ["--film", "--seed", "8", "--film-seconds", "180", "--preroll", "25", "--fps", "30", "--time-scale", "47"]


# Cameras of the film (seed 8, scout of the corrected world) as fixed stills: eye, look, fov (read from the app's audio log, which now carries the fov). A still depends on neither the director nor the simulation,
# so the cases below cannot move when the scout, the traffic or the platform's floating point does (hosted CI once rendered other shots than the ones intended because the scout file was misread).
# The hour is the film's clock at the frame: 17:30 plus (25 s pre-roll + film time) x 47 / 3600.
def film_hour(frame):
    return 17.5 + (25.0 + frame / 30.0) * 47.0 / 3600.0


FILM_CAMERAS = {
    690: ((8.591, -13.42, 3.29), (0.0, -1.488, 1.598), 55.88),
    990: ((5.048, -9.876, 2.524), (0.0, -0.53, 1.406), 46.3),
    1110: ((-0.989, -83.599, 2.92), (-1.736, -98.706, 1.12), 55.0),
    1170: ((-0.922, -97.89, 2.92), (-1.75, -112.639, 1.12), 55.0),
    1221: ((-0.95, -106.242, 2.92), (-1.75, -120.498, 1.12), 55.0),
    1261: ((-0.95, -109.925, 2.92), (-1.75, -123.833, 1.12), 55.0),
}


def film_still(frame):
    eye, look, fov = FILM_CAMERAS[frame]
    return {"still": ",".join("%g" % v for v in (*eye, *look, fov)), "hour": film_hour(frame)}


def clean_env(extra=None):
    """The caller's environment without any SAIGE_ALIVE_* switch (the film's old behaviours and the diagnostics hooks), plus `extra`."""
    env = {k: v for k, v in os.environ.items() if not k.startswith("SAIGE_ALIVE_")}
    if extra:
        env.update(extra)
    return env


def app_extra():
    """Arguments every app run gets: the fixture path, or the humanoid assets."""
    return ["--fixtures"] if FIXTURES else (["--anim", ANIM] if ANIM else [])


class CorruptImage(Exception):
    """A frame the renderer wrote that cannot be a frame: refused, never compared."""


def parse_ppm(data, expect_size=None):
    """Binary PPM (P6): 'P6', width, height, maxval 255, ONE whitespace byte, then exactly width * height * 3 bytes. Anything else (a truncated payload, extra bytes, another maxval, zero or absurd dimensions, a different size than
    expected, a comment line) is a CorruptImage."""
    if not data.startswith(b"P6"):
        raise CorruptImage("not a binary PPM (no P6 magic)")
    pos = 2
    fields = []
    while len(fields) < 3:
        while pos < len(data) and data[pos:pos + 1] in (b" ", b"\t", b"\r", b"\n"):
            pos += 1
        start = pos
        while pos < len(data) and data[pos:pos + 1].isdigit():
            pos += 1
        if start == pos or pos - start > 6:
            raise CorruptImage("malformed PPM header")
        fields.append(int(data[start:pos]))
    if pos >= len(data) or data[pos:pos + 1] not in (b" ", b"\t", b"\r", b"\n"):
        raise CorruptImage("malformed PPM header (no whitespace before the payload)")
    pos += 1
    w, h, maxval = fields
    if w < 1 or h < 1 or w > 16384 or h > 16384:
        raise CorruptImage("PPM dimensions %dx%d out of range" % (w, h))
    if maxval != 255:
        raise CorruptImage("PPM maxval %d (only 255 is written by the renderer)" % maxval)
    if expect_size is not None and (w, h) != tuple(expect_size):
        raise CorruptImage("PPM is %dx%d, expected %dx%d" % (w, h, expect_size[0], expect_size[1]))
    payload = len(data) - pos
    if payload != w * h * 3:
        raise CorruptImage("PPM payload is %d bytes, %dx%d needs %d" % (payload, w, h, w * h * 3))
    return w, h, data[pos:]


def read_ppm(path, expect_size=None):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise CorruptImage("cannot read %s: %s" % (path, e))
    try:
        return parse_ppm(data, expect_size)
    except CorruptImage as e:
        raise CorruptImage("%s: %s" % (path, e))


def run_frames(app, assets, scout, frames, env_extra, outdir, width, height):
    global EXECUTED
    env = clean_env(env_extra)
    os.makedirs(outdir, exist_ok=True)
    cmd = [app, *FILM, *app_extra(), "--scout-file", scout, "--assets", assets, "--width", str(width), "--height", str(height), "--shadow-map", "2048",
           "--frames", ",".join(str(f) for f in frames), "--out", outdir]
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError("renderer failed (%d): %s" % (r.returncode, r.stdout[-400:]))
    EXECUTED += 1
    return {f: read_ppm(os.path.join(outdir, "frame_%05d.ppm" % f), (width, height)) for f in frames}


def run_still(app, assets, spec, env_extra, outdir, width, height):
    """One frame from a fixed camera (independent of the director and the simulation): spec is {"still": "x,y,z,lx,ly,lz,fov", "hour": H}."""
    global EXECUTED
    env = clean_env(env_extra)
    os.makedirs(outdir, exist_ok=True)
    cmd = [app, "--assets", assets, "--width", str(width), "--height", str(height), "--shadow-map", "2048", "--hour", str(spec["hour"]), "--still", spec["still"], "--out", outdir, *app_extra()]
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError("renderer failed (%d): %s" % (r.returncode, r.stdout[-400:]))
    EXECUTED += 1
    return {0: read_ppm(os.path.join(outdir, "still_00.ppm"), (width, height))}


def region_diff(a, b, x0, y0, x1, y1):
    """Fraction of pixels in the region whose channels differ by more than 3/255, and the largest difference."""
    w, h, pa = a
    w2, h2, pb = b
    if (w, h) != (w2, h2):
        raise CorruptImage("compared images differ in size: %dx%d against %dx%d" % (w, h, w2, h2))
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
    """`frames`: a list whose items are film frame numbers (rendered through the director) or still specs ({"still": ..., "hour": ...}, a fixed camera); a single dict is one still."""
    if isinstance(frames, dict):
        frames = [frames]
        regions = {0: regions[0] if 0 in regions else next(iter(regions.values()))}
    worst_frac = 0.0
    for idx, item in enumerate(frames):
        if isinstance(item, dict):
            ref = run_still(args.app, args.assets, item, ref_env, os.path.join(work, "%s_ref_%d" % (name, idx)), width, height)[0]
            out = run_still(args.app, args.assets, item, test_env, os.path.join(work, "%s_test_%d" % (name, idx)), width, height)[0]
            key = idx
        else:
            ref = run_frames(args.app, args.assets, args.scout, [item], ref_env, os.path.join(work, "%s_ref_%d" % (name, idx)), width, height)[item]
            out = run_frames(args.app, args.assets, args.scout, [item], test_env, os.path.join(work, "%s_test_%d" % (name, idx)), width, height)[item]
            key = item
        x0, y0, x1, y1 = regions[key]
        frac, worst = region_diff(ref, out, int(x0 * width), int(y0 * height), int(x1 * width), int(y1 * height))
        print("    %s %s: %.3f%% of the region differs (max channel difference %d)" % (name, ("still %d" % idx) if isinstance(item, dict) else "frame %d" % item, frac * 100.0, worst))
        worst_frac = max(worst_frac, frac)
    return worst_frac


def temporal_case(args, work, mutant_env=None):
    """41 fixed-camera stills along a straight dolly between the film's cameras of frames 1221 and 1261, encoded as a 30 fps clip and swept for structural flicker (frames with >= 2 blocks pulsing)."""
    global EXECUTED
    if shutil.which("ffmpeg") is None:
        raise RuntimeError("ffmpeg is not installed (the temporal case encodes its stills)")
    tag = "mutant" if mutant_env else "ref"
    outdir = os.path.join(work, "clip_stills_%s" % tag)
    os.makedirs(outdir, exist_ok=True)
    (e0, l0, f0), (e1, l1, f1) = FILM_CAMERAS[1221][:3], FILM_CAMERAS[1261][:3]
    specs = []
    for k in range(41):
        u = k / 40.0
        v = [e0[i] + (e1[i] - e0[i]) * u for i in range(3)] + [l0[i] + (l1[i] - l0[i]) * u for i in range(3)] + [f0]
        specs.append(",".join("%.4f" % x for x in v))
    cmd = [args.app, "--assets", args.assets, "--width", "960", "--height", "540", "--shadow-map", "2048", "--hour", "%.4f" % film_hour(1221), "--out", outdir, *app_extra()]
    for sp in specs:
        cmd += ["--still", sp]
    r = subprocess.run(cmd, env=clean_env(mutant_env), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError("renderer failed (%d): %s" % (r.returncode, r.stdout[-300:]))
    EXECUTED += 1
    clip = os.path.join(work, "clip_%s.mp4" % tag)
    r = subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30", "-i", os.path.join(outdir, "still_%02d.ppm"), "-c:v", "libx264", "-crf", "12", "-pix_fmt", "yuv420p", clip], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError("ffmpeg failed: %s" % r.stdout[-300:])
    for k in range(41):   # every still must exist, be a well-formed 960x540 PPM, before the clip is judged
        read_ppm(os.path.join(outdir, "still_%02d.ppm" % k), (960, 540))
    decoded = decoded_frames(clip)
    if decoded != 41:
        raise RuntimeError("the clip has %d decoded frames, expected exactly 41" % decoded)
    n, text = sweep_clip(clip)
    print("    %s: %s" % ("mutant" if mutant_env else "corrected", text))
    return n


def decoded_frames(clip):
    """The number of frames ffprobe really decodes from `clip`."""
    r = subprocess.run(["ffprobe", "-v", "error", "-count_frames", "-select_streams", "v:0", "-show_entries", "stream=nb_read_frames", "-of", "default=nokey=1:noprint_wrappers=1", clip], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0 or not r.stdout.strip().isdigit():
        raise RuntimeError("ffprobe could not count the frames: %s" % r.stdout[-200:])
    return int(r.stdout.strip())


def parse_sweep_output(returncode, output):
    """The structural flicker count from frame_sweep.py's output, or an error: the sweep must exit 0 and print exactly one well-formed non-negative STRUCTURAL line."""
    if returncode != 0:
        raise RuntimeError("frame_sweep exited with status %d: %s" % (returncode, output[-300:]))
    lines = [l for l in output.splitlines() if l.startswith("STRUCTURAL")]
    if len(lines) != 1:
        raise RuntimeError("frame_sweep printed %d STRUCTURAL lines, expected exactly one: %s" % (len(lines), output[-300:]))
    tail = lines[0].rsplit(":", 1)[1].strip()
    if not tail.isdigit():
        raise RuntimeError("frame_sweep's count is not a non-negative integer: '%s'" % tail)
    return int(tail), lines[0]


def sweep_clip(clip):
    sweep = os.path.join(os.path.dirname(os.path.abspath(__file__)), "frame_sweep.py")
    r = subprocess.run([sys.executable, "-I", sweep, clip, "--fps", "30"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return parse_sweep_output(r.returncode, r.stdout)


def required():
    return os.environ.get("SAIGE_REQUIRE_VISUAL") == "1"


def main():
    global ANIM, FIXTURES, EXECUTED
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", required=True)
    ap.add_argument("--assets", default="/tmp/saige-cache")
    ap.add_argument("--anim", default=None, help="the humanoid animation assets (default: the app's own default, /tmp/anim_assets)")
    ap.add_argument("--fixtures", action="store_true", help="generated stand-in assets and the procedural car, no residents: needs nothing downloaded (see the docstring for what it covers)")
    ap.add_argument("--scout", default=None, help="scout report of seed 8 (default: generated with --scout-tool)")
    ap.add_argument("--scout-tool", default=None, help="path of alive_scout, to generate the scout report")
    ap.add_argument("--mutants", action="store_true")
    ap.add_argument("--keep", default=None)
    ap.add_argument("--only", default=None, help="run only the named case (one of: %s)" % ", ".join(KNOWN_CASES))
    ap.add_argument("--tolerance", type=float, default=0.002, help="largest fraction of differing pixels a passing case may have")
    args = ap.parse_args()
    ANIM = args.anim
    FIXTURES = args.fixtures

    # Argument errors are failures, never skips.
    if args.only is not None and args.only not in KNOWN_CASES:
        print("FAILED: unknown case '%s' (known: %s)" % (args.only, ", ".join(KNOWN_CASES)))
        return 2
    if required() and not args.mutants:
        print("FAILED: a required run (SAIGE_REQUIRE_VISUAL=1) must include --mutants: a case that cannot fail proves nothing")
        return 2
    removed = sorted(k for k in os.environ if k.startswith("SAIGE_ALIVE_"))
    if removed:
        print("note: ignoring inherited mutation/diagnostic switches for every render: %s" % ", ".join(removed))

    work = args.keep or tempfile.mkdtemp(prefix="alive_vr_")
    try:
        if FIXTURES:
            args.assets = os.path.join(work, "fixture_assets")
            r = subprocess.run([sys.executable, "-I", os.path.join(os.path.dirname(os.path.abspath(__file__)), "make_fixtures.py"), args.assets], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            print(r.stdout.strip())
            if r.returncode != 0:
                print("FAILED: the fixture assets could not be generated")
                return 1
        if not os.path.exists(args.app) or not os.path.isdir(os.path.join(args.assets, "polyhaven")):
            if required():
                print("FAILED: SAIGE_REQUIRE_VISUAL=1 but the renderer or the Poly Haven texture cache is missing (restore it with tools/restore_assets.py, or use --fixtures)")
                return 1
            print("skipped: renderer or the Poly Haven texture cache is missing")
            return 77
        if args.scout is None:
            if args.scout_tool is None:
                print("FAILED: no scout report (give --scout or --scout-tool)" if required() else "skipped: no scout report")
                return 1 if required() else 77
            args.scout = os.path.join(work, "scout8.txt")
            # The same length the app scouts for the film (preroll 25 + film 180 + 1): the file is then byte-identical to the one saige_alive writes (case scout_producers_agree).
            with open(args.scout, "w") as f:
                subprocess.run([args.scout_tool, "8", "206"], stdout=f, check=True, env=clean_env())
        W, H = 960, 540
        failures = 0
        mutants_expected = 0
        mutants_caught = 0
        cases = [
            # name, frames, region per frame (fractions x0,y0,x1,y1), reference env, test env, mutant env (the film's behaviour)
            # A fixed camera (the film's frame 1245 view: the cafe's side wall from the avenue at a 9 degree sun; hour 18.35), so that the case does not move when the director or the world changes.
            ("interior_never_changes_exterior", {"still": "-0.9501,-118.4359,2.9199,-1.75,-133.011,1.1198,55", "hour": 18.35}, {0: (0.0, 0.0, 0.3, 0.55)}, {"SAIGE_ALIVE_SKIP": "interior"}, {}, {"SAIGE_ALIVE_LEGACY_INTERIOR_WALLS": "1"}),
            ("walls_complete", [film_still(690), film_still(990)], {0: (0.0, 0.0, 1.0, 0.8), 1: (0.0, 0.0, 1.0, 0.8)}, {"SAIGE_ALIVE_ALLFACES": "1"}, {}, {"SAIGE_ALIVE_LEGACY_HIDDEN": "1"}),
            ("scenery_does_not_cover_the_road", [film_still(1110), film_still(1170)], {0: (0.0, 0.58, 1.0, 1.0), 1: (0.0, 0.58, 1.0, 1.0)}, {"SAIGE_ALIVE_NOCOPIES": "1"}, {}, {"SAIGE_ALIVE_LEGACY_SCENERY": "1"}),
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
                mutants_expected += 1
                mutants_caught += 1 if caught else 0
                failures += 0 if caught else 1
        if args.only is None or args.only == "temporal_stability":
            print("case temporal_stability")
            n = temporal_case(args, work)
            ok = n == 0
            print("  %s (%d structural flicker events)" % ("PASS" if ok else "FAIL", n))
            failures += 0 if ok else 1
            if args.mutants:
                nm = temporal_case(args, work, {"SAIGE_ALIVE_LEGACY_INTERIOR_WALLS": "1"})
                print("  mutant (SAIGE_ALIVE_LEGACY_INTERIOR_WALLS): %s (%d events)" % ("CAUGHT" if nm > 0 else "NOT CAUGHT", nm))
                mutants_expected += 1
                mutants_caught += 1 if nm > 0 else 0
                failures += 0 if nm > 0 else 1
        if args.only is None or args.only == "camera_clear_of_foliage":
            # The camera path needs no renderer pass.
            print("case camera_clear_of_foliage")
            cmd = [args.app, *FILM, *app_extra(), "--scout-file", args.scout, "--assets", args.assets, "--width", "320", "--height", "180", "--no-render", "--check-camera"]
            r = subprocess.run(cmd, env=clean_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            EXECUTED += 1
            last = [l for l in r.stdout.splitlines() if l.startswith("camera check")]
            print("    " + (last[-1] if last else r.stdout[-200:]))
            ok = r.returncode == 0
            print("  %s" % ("PASS" if ok else "FAIL"))
            failures += 0 if ok else 1
            if args.mutants:
                r2 = subprocess.run(cmd, env=clean_env({"SAIGE_ALIVE_NO_CANOPY_CLEAR": "1"}), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                EXECUTED += 1
                last = [l for l in r2.stdout.splitlines() if l.startswith("camera check")]
                caught = r2.returncode == 3
                print("  mutant (SAIGE_ALIVE_NO_CANOPY_CLEAR): %s -- %s" % ("CAUGHT" if caught else "NOT CAUGHT", last[-1] if last else ""))
                mutants_expected += 1
                mutants_caught += 1 if caught else 0
                failures += 0 if caught else 1
        if args.only is None or args.only == "scout_producers_agree":
            print("case scout_producers_agree")
            app_made = os.path.join(work, "scout8_app.txt")
            if os.path.exists(app_made):
                os.remove(app_made)
            r = subprocess.run([args.app, *FILM, *app_extra(), "--scout-file", app_made, "--assets", args.assets, "--width", "320", "--height", "180", "--no-render"], env=clean_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            EXECUTED += 1
            same = r.returncode == 0 and os.path.exists(app_made) and open(app_made, "rb").read() == open(args.scout, "rb").read()
            print("  %s (the file saige_alive wrote %s the one alive_scout wrote)" % ("PASS" if same else "FAIL", "equals" if same else "differs from"))
            failures += 0 if same else 1
            if args.mutants:
                # The mutant: the one-line human summary that used to be what `alive_scout 8 230 > scout8.txt` produced. It must be REFUSED by the app, not read as a report.
                human = os.path.join(work, "scout8_human.txt")
                r3 = subprocess.run([args.scout_tool or "alive_scout", "--summary", "8", "206"], stdout=subprocess.PIPE, text=True, env=clean_env()) if args.scout_tool else None
                if r3 is not None and r3.returncode == 0:
                    open(human, "w").write(r3.stdout)
                    r4 = subprocess.run([args.app, *FILM, *app_extra(), "--scout-file", human, "--assets", args.assets, "--width", "320", "--height", "180", "--no-render"], env=clean_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
                    EXECUTED += 1
                    caught = r4.returncode != 0 and "header" in r4.stdout
                    print("  mutant (a human-readable summary as the scout file): %s" % ("CAUGHT (refused)" if caught else "NOT CAUGHT"))
                else:
                    caught = False
                    print("  mutant (a human-readable summary as the scout file): NOT RUN (no alive_scout given)")
                mutants_expected += 1
                mutants_caught += 1 if caught else 0
                failures += 0 if caught else 1
        if EXECUTED == 0:
            print("FAILED: no test executed")
            return 1
        if args.mutants:
            print("mutants: %d of %d intended mutants caught" % (mutants_caught, mutants_expected))
            if mutants_expected == 0 or mutants_caught != mutants_expected:
                failures += 1
        print("visual regression%s: %s (%d renders/checks executed)" % (" (fixtures)" if FIXTURES else "", "ok" if failures == 0 else "%d failure(s)" % failures, EXECUTED))
        return 0 if failures == 0 else 1
    except (RuntimeError, CorruptImage) as e:
        print("FAILED: %s" % e)
        return 1
    finally:
        if args.keep is None:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
