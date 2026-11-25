#!/usr/bin/env python3
"""Showcase 03 soundtrack v2: recorded sound effects (CC0, from the SAIGE asset catalog's OpenGameArt packs) driven by the simulation's own trace, plus the generated pad, birds and crickets
of audio.py for what no recording covers. Mixed offline, deterministically, to a WAV.

  audio_recorded.py TRACE_DIR OUT.wav [seconds] [CACHE_DIR]

Layers and what drives them:
  engine      six recorded engine loops at rising rpm (domasx2, CC0), cross-faded by the vehicle's rpm (equal power in log-rpm) and re-pitched to it, level by throttle and speed
  tires/wind  filtered noise by speed; a recorded wind loop (ignasd, CC0) by speed
  footsteps   Fantozzi's recorded steps (CC0): stone on the town's hard surfaces, sand where the walker is on dirt; one every stride of distance covered, alternating feet, never the same take twice running
  doors       qubodup's door set (CC0): open when the door animation starts, close when the car is entered, open again at the wreck
  crash       recorded breaking, glass, metal hit and rock/debris falling (rubberduck, tinyworlds, CC0) at the recorded impact time, scaled by impact speed, plus a low synthesised boom for the weight no small sample has
"""
import csv, glob, os, sys
from pathlib import Path
import numpy as np
import soundfile as sf

SR = 48000
rng = np.random.default_rng(11)
HERE = Path(__file__).resolve().parent


def load(path, mono=True):
    x, sr = sf.read(str(path), always_2d=True, dtype="float32")
    x = x.mean(axis=1) if mono else x
    if sr != SR:
        from scipy.signal import resample_poly
        from math import gcd
        g = gcd(sr, SR)
        x = resample_poly(x, SR // g, sr // g, axis=0).astype("float32")
    return x


def lowpass(x, fc):
    n = len(x)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1 / SR)
    return np.fft.irfft(X / (1.0 + (f / fc) ** 4), n)


def bandpass(x, lo, hi):
    return lowpass(x - lowpass(x, lo), hi)


def csv_rows(p):
    with open(p) as f:
        return list(csv.DictReader(f))


def place(mix, clip, t, gain=1.0):
    i = int(t * SR)
    if i >= len(mix) or i < 0:
        return
    n = min(len(clip), len(mix) - i)
    mix[i:i + n] += clip[:n] * gain


def looped_resample(buf, rate_per_sample, n):
    """Plays `buf` as a loop with a time-varying playback rate (a vector of rates per output sample), linear interpolation."""
    pos = np.cumsum(rate_per_sample)
    idx = pos % len(buf)
    i0 = idx.astype(np.int64)
    frac = (idx - i0).astype("float32")
    i1 = (i0 + 1) % len(buf)
    return buf[i0] * (1 - frac) + buf[i1] * frac


def main():
    d = sys.argv[1]
    out = sys.argv[2]
    total = float(sys.argv[3]) if len(sys.argv) > 3 else 142.0
    cache = Path(sys.argv[4] if len(sys.argv) > 4 else os.environ.get("SAIGE_ASSET_CACHE", Path.home() / ".cache" / "saige-assets"))
    og = cache / "opengameart"
    n = int(total * SR)
    t = np.arange(n) / SR
    trace = csv_rows(d + "/trace.csv")
    tl = {k: float(v) for k, v in csv_rows(d + "/timeline.csv")[0].items()}
    impacts = csv_rows(d + "/impacts.csv")
    tt = np.array([float(r["t"]) for r in trace])
    speed = np.array([float(r["vehicle_speed"]) for r in trace])
    thr = np.array([float(r["throttle"]) for r in trace])
    rpm = np.array([float(r["rpm"]) for r in trace])
    cs = np.array([float(r["char_speed"]) for r in trace])
    grounded = np.array([float(r["grounded"]) for r in trace])
    driving = np.array([float(r["driving"]) for r in trace])
    crash = impacts and float(impacts[0]["time"]) or tl["crash"]
    mix = np.zeros(n, dtype="float64")
    interp = lambda v: np.interp(t, tt, v)

    # ---- engine: recorded loops cross-faded by rpm
    loops = [load(p) for p in sorted(glob.glob(str(og / "racing_car_engine_sound_loops" / "loop_*.wav")), key=lambda s: (len(s), s))]
    nominal = np.linspace(900.0, 6000.0, len(loops))
    loops = [l / (np.sqrt(np.mean(l ** 2)) + 1e-9) * 0.2 for l in loops]  # equal loudness per layer before the blend
    on = np.minimum(interp(driving * (tt < crash)), 1.0)
    rpm_t = np.clip(interp(rpm), 700, 7000)
    load_t = 0.45 + 0.55 * np.clip(interp(thr), 0, 1)
    spd = np.abs(interp(speed))
    engine = np.zeros(n)
    lr = np.log(nominal)
    pos_in_layers = np.interp(np.log(rpm_t), lr, np.arange(len(loops)))  # fractional layer index
    for i, loop in enumerate(loops):
        w = np.clip(1.0 - np.abs(pos_in_layers - i), 0.0, 1.0)  # triangular in log-rpm: neighbouring weights sum to 1
        gain = np.sin(0.5 * np.pi * w)  # equal power across the pair
        if not np.any(gain > 1e-3):
            continue
        rate = rpm_t / nominal[i]
        engine += gain * looped_resample(loop, rate, n)
    engine = lowpass(engine, 3500)
    mix += on * engine * load_t * 0.40

    # ---- tires and wind
    tire = bandpass(rng.standard_normal(n), 300, 2500) * np.clip(spd / 25.0, 0, 1) ** 1.5
    mix += on * tire * 0.06
    wind_files = sorted(glob.glob(str(og / "wind" / "contents" / "wind" / "Wind*.ogg"))) or sorted(glob.glob(str(og / "wind" / "**" / "Wind*.ogg"), recursive=True))
    if wind_files:
        w = load(wind_files[0])
        w = w / (np.sqrt(np.mean(w ** 2)) + 1e-9) * 0.15
        wl = looped_resample(w, np.ones(n), n)
        mix += wl * (0.25 + 0.75 * np.clip(interp(np.abs(speed) / 30.0), 0, 1)) * 0.30

    # ---- footsteps: distance-triggered, surface by place (the town's hard ground, then dirt), alternating feet, no immediate repeats
    steps_dir = og / "fantozzis_footsteps_grasssand_stone" / "contents"
    flacs = {p.stem: p for p in steps_dir.rglob("*.flac")}
    banks = {surf: {foot: [load(p) for k, p in sorted(flacs.items()) if k.startswith(f"Fantozzi-{surf}{foot}")] for foot in "LR"} for surf in ("Stone", "Sand")}
    pos, nxt, foot, last = 0.0, 0.5, 0, None
    dt = np.diff(tt, prepend=tt[0])
    nsteps = 0
    for i in range(len(tt)):
        if driving[i] < 0.5 and grounded[i] > 0.5 and cs[i] > 0.25:
            pos += cs[i] * dt[i]
            if pos >= nxt:
                surf = "Stone" if tt[i] < tl["enter"] + 1 else "Sand"
                takes = banks[surf]["LR"[foot]]
                k = int(rng.integers(len(takes)))
                if k == last:
                    k = (k + 1) % len(takes)
                last = k
                clip = takes[k] / (np.max(np.abs(takes[k])) + 1e-9)
                place(mix, clip * float(0.5 + 0.12 * min(cs[i], 7.0)) * float(rng.uniform(0.85, 1.0)), tt[i])
                foot ^= 1
                nxt = pos + 0.78 + 0.1 * min(cs[i], 7.0)
                nsteps += 1

    # ---- doors
    doors = sorted((og / "door_open_door_close_set" / "contents").rglob("*.flac"))
    opens = [load(p) for p in doors if "Open" in p.stem]
    closes = [load(p) for p in doors if "Close" in p.stem]
    def door(clips, tm, g):
        c = clips[int(rng.integers(len(clips)))]
        place(mix, c / (np.max(np.abs(c)) + 1e-9), tm, g)
    door(opens, tl["enter"] + 0.2, 0.55)
    door(closes, tl["drive"] - 0.3, 0.7)
    door(opens, tl["exit"] + 0.2, 0.55)
    door(closes, tl["exit"] + 2.9, 0.45)

    # ---- the crash: recorded layers at the impact, scaled by its speed, and a synthesised sub boom
    for e in impacts:
        ti, sp = float(e["time"]), float(e["speed"])
        k = min(1.0, sp / 22.0)
        pick = lambda pat, m=3: [load(p) for p in sorted((og / "75_cc0_breaking_falling_hit_sfx" / "contents").glob(pat))][:m]
        for clip, g, dl in [*[(c, 0.9, 0.0) for c in pick("bfh1_metal_hit_*.ogg", 2)], *[(c, 0.7, 0.03) for c in pick("bfh1_glass_breaking_*.ogg", 2)], *[(c, 0.55, 0.12) for c in pick("bfh1_rock_breaking_*.ogg", 2)]]:
            place(mix, clip / (np.max(np.abs(clip)) + 1e-9), ti + dl, 1.5 * g * k)
        glass = load(og / "glass_break" / "glass_breaking.wav")
        place(mix, glass / (np.max(np.abs(glass)) + 1e-9), ti + 0.05, 0.6 * k)
        debris = pick("bfh1_rock_falling_*.ogg", 9) + pick("bfh1_metal_falling_*.ogg", 5)
        for _ in range(16):
            c = debris[int(rng.integers(len(debris)))]
            place(mix, c / (np.max(np.abs(c)) + 1e-9), ti + float(rng.uniform(0.2, 3.0)), float(rng.uniform(0.15, 0.4)) * k)
        m = min(int(3.2 * SR), n - int(ti * SR))
        tau = np.arange(m) / SR
        boom = np.sin(2 * np.pi * (46 + 18 * np.exp(-tau * 9)) * tau) * np.exp(-tau * 5.0)
        mix[int(ti * SR): int(ti * SR) + m] += boom * 1.2 * k

    # ---- generated bed: pad, birds at dawn, crickets at night (no recordings of these were available)
    sys.path.insert(0, str(HERE))
    chords = [(57, 60, 64), (53, 57, 60), (55, 59, 62), (52, 55, 59), (57, 60, 64), (50, 53, 57), (52, 55, 59), (57, 61, 64), (45, 52, 57)]
    pad = np.zeros(n)
    for idx, ch in enumerate(chords):
        s0, s1 = idx * 16.0, idx * 16.0 + 20.0
        i0, i1 = int(s0 * SR), min(int(s1 * SR), n)
        if i0 >= n:
            break
        seg = np.arange(i1 - i0) / SR
        env = np.clip(seg / 4.0, 0, 1) * np.clip((20.0 - seg) / 4.0, 0, 1)
        for note in ch:
            f = 440.0 * 2 ** ((note - 69) / 12.0)
            for det in (-0.004, 0.0, 0.004):
                pad[i0:i1] += env * (np.sin(2 * np.pi * f * (1 + det) * seg) + 0.35 * np.sin(4 * np.pi * f * (1 + det) * seg))
    pad = lowpass(pad, 1400)
    level = np.interp(t, [0, 8, 30, 47, 60, 88, 91, 93, 100, 125, 140, 142], [0.0, 0.05, 0.07, 0.10, 0.14, 0.14, 0.02, 0.05, 0.07, 0.08, 0.05, 0.0])
    mix += pad * level * 0.45
    cricket_gate = np.clip((t - 126.0) / 8.0, 0, 1)
    mix += np.sin(2 * np.pi * 4300 * t) * (np.sin(2 * np.pi * 38 * t) > 0.2) * (np.sin(2 * np.pi * 3.1 * t) > -0.3) * cricket_gate * 0.012
    for _ in range(60):
        s = rng.uniform(0.5, 30.0)
        base = rng.uniform(2800, 4200)
        for j in range(int(rng.integers(2, 5))):
            i0 = int((s + j * 0.11) * SR)
            m = int(0.09 * SR)
            if i0 + m < n:
                tau = np.arange(m) / SR
                f = base + (base * 0.25) * tau / 0.09
                mix[i0:i0 + m] += 0.012 * (1 - s / 40) * np.sin(np.pi * tau / 0.09) ** 2 * np.sin(2 * np.pi * np.cumsum(f) / SR)

    mix *= np.clip(t / 1.5, 0, 1) * np.clip((total - t) / 3.0, 0, 1)
    mix = np.tanh(mix * 1.1) / np.tanh(1.1)
    mix /= max(1e-9, np.max(np.abs(mix))) / 0.89
    pcm = (np.stack([mix, np.roll(mix, 37)], axis=1) * 32767).astype("<i2")
    sf.write(out, pcm, SR, subtype="PCM_16")
    print(f"wrote {out}: {total:.1f} s, {nsteps} recorded footsteps, engine layers {len(loops)}, impacts {len(impacts)}")


main()
