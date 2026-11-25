#!/usr/bin/env python3
"""Audio preparation for game use: trim, loudness-normalise, build a seamless loop, fade, resample, encode, and write metadata.

  audio_prep.py IN OUT [--trim-db -55] [--lufs -23] [--peak-db -1] [--loop] [--loop-fade 0.25] [--fade-in 0.005] [--fade-out 0.02] [--rate 48000] [--mono] [--codec opus|vorbis|flac|wav] [--bitrate 96k] [--meta OUT.json]

Loudness: integrated loudness (ITU BS.1770 via pyloudnorm) to the target, then a true-peak-safe ceiling. A one-shot (an impact, a footstep) should be normalised by peak, not loudness:
use `--lufs none --peak-db -3` for those. Loops: the tail is crossfaded into the head over `--loop-fade` seconds (equal-power) and the result is shortened by that much, so the file
loops with no click and no level jump; the seam is measured and reported. 3D sounds are made mono (`--mono`): the engine's spatializer pans them.
"""
import argparse, json, subprocess, sys, tempfile, os
from pathlib import Path
import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from saige_assets import extract  # noqa: E402


def load(path):
    try:
        x, sr = sf.read(path, always_2d=True, dtype="float32")
    except Exception:
        tmp = tempfile.mktemp(suffix=".wav")
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(path), tmp], check=True)
        x, sr = sf.read(tmp, always_2d=True, dtype="float32")
        os.unlink(tmp)
    return x, sr


def trim(x, sr, thr_db):
    thr = 10 ** (thr_db / 20)
    level = np.max(np.abs(x), axis=1)
    idx = np.nonzero(level > thr)[0]
    if idx.size == 0:
        return x
    pad = int(0.003 * sr)
    return x[max(0, idx[0] - pad): idx[-1] + 1 + pad]


def resample(x, sr, rate):
    if sr == rate:
        return x, sr
    from scipy.signal import resample_poly
    from math import gcd
    g = gcd(sr, rate)
    return resample_poly(x, rate // g, sr // g, axis=0).astype("float32"), rate


def normalise(x, sr, lufs, peak_db):
    if lufs is not None and len(x) / sr >= 0.5:
        import pyloudnorm as pyln
        meter = pyln.Meter(sr)
        cur = meter.integrated_loudness(x if x.shape[1] <= 2 else x.mean(axis=1))
        if np.isfinite(cur):
            x = x * 10 ** ((lufs - cur) / 20)
    peak = float(np.max(np.abs(x))) or 1.0
    ceiling = 10 ** (peak_db / 20)
    if peak > ceiling or lufs is None:
        x = x * (ceiling / peak)
    return x


def make_loop(x, sr, fade_s):
    n = int(fade_s * sr)
    if len(x) < 4 * n:
        raise SystemExit("clip too short for the requested loop crossfade")
    head, tail = x[:n], x[-n:]
    t = np.linspace(0, np.pi / 2, n, dtype="float32")[:, None]
    blended = tail * np.cos(t) + head * np.sin(t)  # equal power: the tail fades out under the head fading in
    return np.concatenate([x[n:-n], blended]), n


def fades(x, sr, fin, fout):
    x = x.copy()
    a, b = int(fin * sr), int(fout * sr)
    if a:
        x[:a] *= np.linspace(0, 1, a, dtype="float32")[:, None]
    if b:
        x[-b:] *= np.linspace(1, 0, b, dtype="float32")[:, None]
    return x


def encode(x, sr, out, codec, bitrate):
    out = Path(out)
    if codec == "wav":
        sf.write(out, x, sr, subtype="PCM_16")
        return
    tmp = tempfile.mktemp(suffix=".wav")
    sf.write(tmp, x, sr, subtype="FLOAT")
    args = {"opus": ["-c:a", "libopus", "-b:a", bitrate], "vorbis": ["-c:a", "libvorbis", "-b:a", bitrate], "flac": ["-c:a", "flac"]}[codec]
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", tmp, *args, str(out)], check=True)
    os.unlink(tmp)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input"); ap.add_argument("output")
    ap.add_argument("--trim-db", type=float, default=-55.0); ap.add_argument("--no-trim", action="store_true")
    ap.add_argument("--lufs", default="-23"); ap.add_argument("--peak-db", type=float, default=-1.0)
    ap.add_argument("--loop", action="store_true"); ap.add_argument("--loop-fade", type=float, default=0.25)
    ap.add_argument("--fade-in", type=float, default=0.004); ap.add_argument("--fade-out", type=float, default=0.02)
    ap.add_argument("--rate", type=int, default=48000); ap.add_argument("--mono", action="store_true")
    ap.add_argument("--codec", default="opus", choices=["opus", "vorbis", "flac", "wav"]); ap.add_argument("--bitrate", default="96k")
    ap.add_argument("--meta")
    a = ap.parse_args()
    x, sr = load(a.input)
    src_dur = len(x) / sr
    if a.mono and x.shape[1] > 1:
        x = x.mean(axis=1, keepdims=True)
    if not a.no_trim:
        x = trim(x, sr, a.trim_db)
    x, sr = resample(x, sr, a.rate)
    lufs = None if a.lufs == "none" else float(a.lufs)
    x = normalise(x, sr, lufs, a.peak_db)
    seam = None
    if a.loop:
        x, n = make_loop(x, sr, a.loop_fade)
        seam = float(np.max(np.abs(x[0] - x[-1])))
    else:
        x = fades(x, sr, a.fade_in, a.fade_out)
    encode(x, sr, a.output, a.codec, a.bitrate)
    meta = {"source": str(a.input), "output": str(a.output), "codec": a.codec, "sample_rate": sr, "channels": x.shape[1], "source_duration_s": round(src_dur, 3), "duration_s": round(len(x) / sr, 3),
            "loop": a.loop, "loop_seam_abs_diff": None if seam is None else round(seam, 5), "target_lufs": lufs, "peak_ceiling_db": a.peak_db, "bytes": Path(a.output).stat().st_size}
    meta["analysis"] = extract.describe_audio(a.output) if a.codec != "opus" else extract.describe_audio(a.output)
    if a.meta:
        Path(a.meta).write_text(json.dumps(meta, indent=1))
    print(json.dumps(meta, indent=1))


main()
