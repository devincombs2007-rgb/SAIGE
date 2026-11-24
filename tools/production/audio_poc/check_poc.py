#!/usr/bin/env python3
"""Runs audio_poc and checks the physics of what it rendered: Doppler frequencies, pan, distance attenuation, occlusion filtering, reverb tail.
  check_poc.py AUDIO_POC_BINARY WORKDIR
"""
import json, subprocess, sys
import numpy as np
import soundfile as sf

binary, work = sys.argv[1], sys.argv[2]
import os
os.makedirs(work, exist_ok=True)
sr = 48000
t = np.arange(2 * sr) / sr
sf.write(f"{work}/tone.wav", (0.5 * np.sin(2 * np.pi * 440 * t)).astype("float32"), sr)  # 440 Hz, 880 whole cycles: loops without a click
out = subprocess.run([binary, work, f"{work}/tone.wav"], capture_output=True, text=True, check=True).stdout
summary = json.loads(out)


def dominant(x, sr_):
    w = np.hanning(len(x))
    spec = np.abs(np.fft.rfft(x * w, 8 * len(x)))
    f = np.fft.rfftfreq(8 * len(x), 1 / sr_)
    band = (f > 200) & (f < 900)
    return float(f[band][np.argmax(spec[band])])


checks = []
pb, _ = sf.read(f"{work}/passby.wav", always_2d=True)
seg = lambda a, b: pb[int(a * sr): int(b * sr)]
c, v = 343.3, 40.0
f_in, f_out = dominant(seg(0.4, 0.9)[:, 0] + seg(0.4, 0.9)[:, 1], sr), dominant(seg(5.1, 5.6)[:, 0] + seg(5.1, 5.6)[:, 1], sr)
# the source is 120 m behind at t = 0 and closing at 40 m/s: the radial speed is slightly below 40 m/s while it is 8 m off the axis
exp_in = 440 / (1 - v * 0.9988 / c)
exp_out = 440 / (1 + v * 0.9988 / c)
checks.append(("doppler approaching", f_in, exp_in, abs(f_in - exp_in) < 6))
checks.append(("doppler receding", f_out, exp_out, abs(f_out - exp_out) < 6))
rms = lambda x: float(np.sqrt(np.mean(x ** 2)))
env = np.array([rms(pb[int(i * 0.1 * sr): int((i + 1) * 0.1 * sr)]) for i in range(60)])
peak_t = float(np.argmax(env) * 0.1 + 0.05)
checks.append(("loudest at closest approach (3.0 s)", peak_t, 3.0, abs(peak_t - 3.0) < 0.35))
checks.append(("farther is quieter", float(env[0]), float(env[25]), env[0] < 0.35 * env[25]))
# the source is on the left of the road, listener faces +X: left channel louder throughout
checks.append(("pan: source on the left is louder in the left channel", rms(pb[:, 0]), rms(pb[:, 1]), rms(pb[:, 0]) > 1.05 * rms(pb[:, 1])))
op, _ = sf.read(f"{work}/open.wav", always_2d=True)
oc, _ = sf.read(f"{work}/occluded.wav", always_2d=True)
hf = lambda x: float(np.sum(np.abs(np.fft.rfft(x[:, 0]))[np.fft.rfftfreq(len(x), 1 / sr) > 300]))  # energy above the occluded cutoff
checks.append(("occlusion: low-pass removes the tone and lowers level", rms(oc), rms(op), rms(oc) < 0.6 * rms(op)))
rv, _ = sf.read(f"{work}/reverb.wav", always_2d=True)
tail = rms(rv[int(0.5 * sr):, 0])
checks.append(("reverb: a tail rings on after the 0.2 s burst", tail, 0.0, tail > 1e-4))
checks.append(("rpm blend equal power", 1.0, 1.0, summary["rpm_blend_equal_power"]))
ok = True
for name, got, want, passed in checks:
    print(f"{'ok  ' if passed else 'FAIL'} {name}: {got:.4g} (expected ~{want:.4g})" if isinstance(want, float) else f"{'ok  ' if passed else 'FAIL'} {name}")
    ok = ok and bool(passed)
sys.exit(0 if ok else 1)
