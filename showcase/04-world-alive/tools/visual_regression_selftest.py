#!/usr/bin/env python3
"""Negative tests for tools/visual_regression.py: the runner's own failure modes. A visual test that cannot fail on bad input proves nothing, so each way the runner could be fooled is fed to it here and must be refused:
corrupt or truncated frames, images of the wrong size, a sweep that exits non-zero or prints nothing usable, a clip with the wrong number of frames, unknown cases, a required run without mutants, an empty or invalid seed list.
Needs no renderer and no assets. Exit status 0 when every case was refused as required."""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("visual_regression", os.path.join(HERE, "visual_regression.py"))
vr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vr)

failures = 0


def check(cond, what):
    global failures
    if not cond:
        print("FAIL: " + what)
        failures += 1


def refused(fn, *a):
    try:
        fn(*a)
    except (vr.CorruptImage, RuntimeError):
        return True
    return False


def ppm(w, h, payload_len=None, maxval=255, magic=b"P6", sep=b"\n", comment=b""):
    n = w * h * 3 if payload_len is None else payload_len
    return magic + b" " + comment + b"%d %d %d" % (w, h, maxval) + sep + bytes(n)


# PPM parsing
good = ppm(2, 2)
check(vr.parse_ppm(good)[:2] == (2, 2), "a well-formed 2x2 PPM was refused")
check(vr.parse_ppm(good, (2, 2))[:2] == (2, 2), "a well-formed PPM of the expected size was refused")
for name, data in [
    ("truncated payload", ppm(2, 2, payload_len=11)),
    ("extra bytes", ppm(2, 2, payload_len=13)),
    ("header only", b"P6\n2 2\n255\n"),
    ("empty file", b""),
    ("wrong magic (P3)", ppm(2, 2, magic=b"P3")),
    ("maxval 65535", ppm(2, 2, maxval=65535)),
    ("maxval 0", ppm(2, 2, maxval=0)),
    ("zero width", ppm(0, 2)),
    ("zero height", ppm(2, 0, payload_len=0)),
    ("absurd width", b"P6\n99999 2\n255\n" + bytes(6)),
    ("no whitespace before the payload", b"P6\n2 2\n255" + bytes(12)),
    ("non-numeric header", b"P6\nab 2\n255\n" + bytes(12)),
    ("missing field", b"P6\n2\n255\n" + bytes(12)),
]:
    check(refused(vr.parse_ppm, data), "PPM case '%s' was accepted" % name)
check(refused(vr.parse_ppm, ppm(3, 2), (2, 2)), "a 3x2 PPM was accepted where 2x2 was expected")
d = tempfile.mkdtemp(prefix="vr_selftest_")
try:
    p = os.path.join(d, "x.ppm")
    open(p, "wb").write(ppm(2, 2, payload_len=5))
    check(refused(vr.read_ppm, p), "a truncated file on disk was accepted")
    check(refused(vr.read_ppm, os.path.join(d, "missing.ppm")), "a missing file was accepted")
finally:
    shutil.rmtree(d, ignore_errors=True)
# comparing images of different sizes
check(refused(vr.region_diff, (2, 2, bytes(12)), (3, 2, bytes(18)), 0, 0, 2, 2), "images of different sizes were compared")
# the sweep's verdict
check(vr.parse_sweep_output(0, "x\nSTRUCTURAL flicker events (>= 2 blocks in which more than 25% of the pixels pulse): 4\n")[0] == 4, "a good sweep line was refused")
check(vr.parse_sweep_output(0, "STRUCTURAL flicker events (...): 0\n")[0] == 0, "a zero count was refused")
for name, rc, out in [
    ("non-zero exit", 1, "STRUCTURAL flicker events (...): 0\n"),
    ("crash", -11, ""),
    ("no STRUCTURAL line", 0, "frames: 41\n"),
    ("two STRUCTURAL lines", 0, "STRUCTURAL a: 0\nSTRUCTURAL b: 0\n"),
    ("negative count", 0, "STRUCTURAL flicker events (...): -1\n"),
    ("non-numeric count", 0, "STRUCTURAL flicker events (...): many\n"),
    ("empty output", 0, ""),
]:
    check(refused(vr.parse_sweep_output, rc, out), "sweep case '%s' was accepted" % name)
# the frame count of a clip
if shutil.which("ffprobe") and shutil.which("ffmpeg"):
    d = tempfile.mkdtemp(prefix="vr_selftest_")
    try:
        for k in range(5):
            open(os.path.join(d, "f_%02d.ppm" % k), "wb").write(ppm(16, 16))
        clip = os.path.join(d, "c.mp4")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", "30", "-i", os.path.join(d, "f_%02d.ppm"), "-c:v", "libx264", "-pix_fmt", "yuv420p", clip], check=True)
        check(vr.decoded_frames(clip) == 5, "ffprobe did not report 5 decoded frames for a 5-frame clip")
        check(vr.decoded_frames(clip) != 41, "a 5-frame clip would pass as 41")
        check(refused(vr.decoded_frames, os.path.join(d, "no_such.mp4")), "a missing clip was counted")
    finally:
        shutil.rmtree(d, ignore_errors=True)
else:
    print("note: ffmpeg/ffprobe not installed, the clip frame count case is skipped")
# the command line
def run(*args, env=None):
    e = {k: v for k, v in os.environ.items() if k != "SAIGE_REQUIRE_VISUAL"}
    if env:
        e.update(env)
    return subprocess.run([sys.executable, "-I", os.path.join(HERE, "visual_regression.py"), "--app", "/nonexistent", *args], env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True).returncode
check(run("--only", "no_such_case") == 2, "an unknown --only was accepted")
check(run(env={"SAIGE_REQUIRE_VISUAL": "1"}) == 2, "a required run without --mutants was accepted")
check(run(env={"SAIGE_REQUIRE_VISUAL": "1"}) != 0, "a required run did not fail")
check(run("--fixtures", "--mutants", env={"SAIGE_REQUIRE_VISUAL": "1"}) == 1, "a required run with no renderer did not fail with status 1")
check(run("--fixtures") == 77, "a non-required run with no renderer was not a skip")
seeds = [sys.executable, "-I", os.path.join(HERE, "camera_seeds.py"), "--app", "/nonexistent", "--scout", "/nonexistent"]
for bad in ["", "1,x", "1,1", ","]:
    check(subprocess.run(seeds + ["--seeds", bad], stdout=subprocess.PIPE, stderr=subprocess.STDOUT).returncode == 2, "camera_seeds accepted --seeds '%s'" % bad)

if failures == 0:
    print("visual runner self-test passed")
sys.exit(1 if failures else 0)
