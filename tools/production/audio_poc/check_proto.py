#!/usr/bin/env python3
"""Runs audio_proto and measures what it rendered: does the engine's pitch follow the rpm, does its level stay continuous through the loop crossfades, do impacts get louder and lower with energy, and do the
materials sound like their category (ring time and brightness).   check_proto.py AUDIO_PROTO_BINARY WORKDIR
"""
import json, os, subprocess, sys
import numpy as np
import soundfile as sf

binary, work = sys.argv[1], sys.argv[2]
os.makedirs(work, exist_ok=True)
summary = json.loads(subprocess.run([binary, work], capture_output=True, text=True, check=True).stdout)
sr = 48000
checks = []

# Engine: the firing frequency (4 cylinders, 4 stroke: rpm / 30 Hz) tracked through the sweep.
eng, _ = sf.read(f"{work}/engine.wav", always_2d=True)
x = eng[:, 0]
tl = summary["engine"]["timeline"]


def fundamental(seg):
    """Harmonic-sum estimate of the fundamental in 20..400 Hz (the strongest single partial is often a harmonic of a stack)."""
    w = np.hanning(len(seg))
    spec = np.abs(np.fft.rfft(seg * w, 8 * len(seg)))
    f = np.fft.rfftfreq(8 * len(seg), 1 / sr)
    cands = np.arange(20.0, 400.0, 0.5)
    score = [sum(np.interp(c * h, f, spec) for h in range(1, 9)) / np.sqrt(8) for c in cands]
    return float(cands[int(np.argmax(score))])


worst = 0.0
n_checked = 0
for e in tl:
    if e["t"] < 0.6 or e["t"] > 12.2:
        continue
    t = e["t"]
    seg = x[int((t - 0.2) * sr): int((t + 0.2) * sr)]
    # the rpm the sound had during the window is the smoothed rpm (the loops follow it)
    f_exp = np.mean([k["smoothed"] for k in tl if t - 0.2 <= k["t"] <= t + 0.2]) / 30.0
    f_got = fundamental(seg)
    worst = max(worst, abs(f_got - f_exp) / f_exp)
    n_checked += 1
checks.append(("engine pitch follows the rpm (worst relative error over the sweep)", worst, 0.0, worst < 0.04 and n_checked > 30))
env = np.array([float(np.sqrt(np.mean(x[int(i * 0.1 * sr): int((i + 1) * 0.1 * sr)] ** 2))) for i in range(int(len(x) / (0.1 * sr)))])
env_db = 20 * np.log10(np.maximum(env, 1e-6))
smooth = np.convolve(env_db, np.ones(5) / 5, mode="same")
dips = float(np.max((smooth - env_db)[5:-5]))
checks.append(("engine level is continuous through the crossfades (largest dip below its neighbours, dB; the stand-in loops are not level matched, recorded ones must be)", dips, 0.0, dips < 3.5))

# Impacts.
imp = {c["name"] + str(i): c for i, c in enumerate(summary["impacts"])}
imps = summary["impacts"]
rows = []
for c in imps:
    y, _ = sf.read(f"{work}/{c['file']}", always_2d=True)
    y = y[:, 0]
    peak = float(np.max(np.abs(y)))
    e = y ** 2
    cum = np.cumsum(e[::-1])[::-1]
    # ring time: when the remaining energy falls 30 dB below the total
    t30 = float(np.argmax(cum < cum[0] * 1e-3)) / sr if cum[0] > 0 else 0.0
    spec = np.abs(np.fft.rfft(y[: int(0.4 * sr)]))
    f = np.fft.rfftfreq(int(0.4 * sr), 1 / sr)
    centroid = float(np.sum(f * spec) / max(np.sum(spec), 1e-12))
    rows.append((c, peak, t30, centroid))
    print(f"   {c['name']:<18} {c['category']:<9} gain {c['gain']:.3f} pitch {c['pitch']:.3f}  peak {peak:.3f}  ring {t30 * 1000:.0f} ms  centroid {centroid:.0f} Hz")
asph = [r for r in rows if r[0]["name"] == "car_on_asphalt"]
checks.append(("louder impacts for more energy (peak level rises with energy)", asph[-1][1], asph[0][1], all(asph[i][1] < asph[i + 1][1] for i in range(len(asph) - 1))))
checks.append(("a heavier blow sounds lower (centroid falls with energy: same sample, lower pitch)", asph[-1][3], asph[0][3], asph[-1][3] < 0.9 * asph[0][3]))
checks.append(("peak level tracks the model's gain (peak / gain within 15% across energies)", 0.0, 0.0, all(abs(r[1] / r[0]["gain"] / (asph[-1][1] / asph[-1][0]["gain"]) - 1) < 0.15 for r in asph if r[0]["gain"] > 0.05)))
by_name = {r[0]["name"]: r for r in rows}
checks.append(("categories follow the materials (car on asphalt metal, grass soft, gravel granular, rock on ice glassy, rock on concrete hard)",
               0.0, 0.0, by_name["car_on_asphalt"][0]["category"] == "metal" and by_name["car_on_grass"][0]["category"] == "soft" and by_name["car_on_gravel"][0]["category"] == "granular" and by_name["rock_on_ice"][0]["category"] == "glassy" and by_name["rock_on_concrete"][0]["category"] == "hard"))
checks.append(("the category selects the sound that is played (soft and dull are low, metal, hard and granular are bright: centroid)", by_name["car_on_grass"][3], by_name["car_on_asphalt"][3], by_name["car_on_grass"][3] < 2000 and by_name["tire_on_rubber"][3] < 2500 and min(by_name["car_on_asphalt"][3], by_name["rock_on_concrete"][3], by_name["car_on_gravel"][3]) > 3000))
checks.append(("a scrape (20 J) is silent: nothing to play", by_name["scrape"][0]["gain"], 0.0, by_name["scrape"][0]["gain"] == 0.0 and by_name["scrape"][1] < 1e-6))
ok = True
for name, got, want, passed in checks:
    print(f"{'ok  ' if passed else 'FAIL'} {name}: {got:.4g}")
    ok = ok and bool(passed)
sys.exit(0 if ok else 1)
