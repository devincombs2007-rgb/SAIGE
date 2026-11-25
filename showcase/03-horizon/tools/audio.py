#!/usr/bin/env python3
"""Synthesises the Showcase 03 soundtrack from the simulation's own trace (saige_horizon_sim_run --out DIR writes trace.csv, impacts.csv, timeline.csv).

Every sound is generated here from the numbers the simulation produced: the engine note follows the vehicle's rpm and speed, tire and wind noise follow its speed, the
footsteps follow the distance the character covers, the door sounds follow the door phases and the crash follows the recorded impact event. The score is a simple
generated pad. Nothing is sampled or licensed; there are no recordings.

  audio.py TRACE_DIR OUT.wav [seconds]
"""
import csv, sys
import numpy as np

SR = 44100
rng = np.random.default_rng(7)


def load(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def one_pole_lp(x, fc):
    a = np.exp(-2 * np.pi * fc / SR)
    y = np.empty_like(x)
    acc = 0.0
    for i in range(len(x)):  # slow but fine for a few million samples with numpy vectorisation below
        acc = a * acc + (1 - a) * x[i]
        y[i] = acc
    return y


def lowpass(x, fc):
    # FFT brick-wall with a smooth roll-off: quick and good enough for noise shaping
    n = len(x)
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1 / SR)
    X *= 1.0 / (1.0 + (f / fc) ** 4)
    return np.fft.irfft(X, n)


def highpass(x, fc):
    return x - lowpass(x, fc)


def bandpass(x, lo, hi):
    return lowpass(highpass(x, lo), hi)


def interp_track(times, values, n):
    t = np.arange(n) / SR
    return np.interp(t, times, values)


def main():
    d = sys.argv[1]
    out = sys.argv[2]
    trace = load(d + '/trace.csv')
    impacts = load(d + '/impacts.csv')
    tl = {k: float(v) for k, v in load(d + '/timeline.csv')[0].items()}
    total = float(sys.argv[3]) if len(sys.argv) > 3 else 142.0
    n = int(total * SR)
    t = np.arange(n) / SR
    tt = np.array([float(r['t']) for r in trace])
    speed = np.array([float(r['vehicle_speed']) for r in trace])
    thr = np.array([float(r['throttle']) for r in trace])
    rpm = np.array([float(r['rpm']) for r in trace])
    cs = np.array([float(r['char_speed']) for r in trace])
    grounded = np.array([float(r['grounded']) for r in trace])
    driving = np.array([float(r['driving']) for r in trace])
    crash = tl['crash'] if tl['crash'] > 0 else 91.3
    mix = np.zeros(n)

    # --- Wind: band-limited noise whose level rises with the car's speed and eases at the end.
    wind = bandpass(rng.standard_normal(n), 120, 1800)
    wind_level = 0.05 + 0.10 * interp_track(tt, np.clip(np.abs(speed) / 25.0, 0, 1) * driving, n) + 0.02 * np.sin(t * 0.37) ** 2
    mix += wind * wind_level * 1.0

    # --- Dawn birds before the walker sets off, crickets once it is dark.
    def chirp(start, f0, f1, dur, amp):
        i0 = int(start * SR)
        m = int(dur * SR)
        if i0 + m >= n:
            return
        tau = np.arange(m) / SR
        f = f0 + (f1 - f0) * tau / dur
        env = np.sin(np.pi * tau / dur) ** 2
        mix[i0:i0 + m] += amp * env * np.sin(2 * np.pi * np.cumsum(f) / SR)
    for k in range(60):
        s = rng.uniform(0.5, 30.0)
        base = rng.uniform(2800, 4200)
        for j in range(rng.integers(2, 5)):
            chirp(s + j * 0.11, base, base * rng.uniform(1.1, 1.4), 0.09, 0.012 * (1.0 - s / 40.0))
    cricket_gate = np.clip((t - 126.0) / 8.0, 0, 1)
    cr = np.sin(2 * np.pi * 4300 * t) * (np.sin(2 * np.pi * 38 * t) > 0.2) * (np.sin(2 * np.pi * 3.1 * t) > -0.3)
    mix += cr * cricket_gate * 0.012

    # --- Footsteps: one every 0.78 m at a walk, up to 1.4 m at a sprint, a soft thud plus a grit tick.
    pos = 0.0
    nxt = 0.5
    dt = np.diff(tt, prepend=tt[0])
    steps = []
    for i in range(len(tt)):
        if driving[i] < 0.5 and grounded[i] > 0.5 and cs[i] > 0.25:
            pos += cs[i] * dt[i]
            stride = 0.78 + 0.1 * min(cs[i], 7.0)
            if pos >= nxt:
                steps.append((tt[i], cs[i]))
                nxt = pos + stride
    for s, v in steps:
        i0 = int(s * SR)
        m = int(0.16 * SR)
        if i0 + m >= n:
            continue
        tau = np.arange(m) / SR
        thud = np.sin(2 * np.pi * (95 + 25 * rng.random()) * tau) * np.exp(-tau * 38)
        grit = bandpass(rng.standard_normal(m), 1800, 6000) * np.exp(-tau * 60) * 0.35
        mix[i0:i0 + m] += (thud * 0.5 + grit) * min(0.1 + 0.07 * v, 0.5)

    # --- Doors: the latch click and the thunk of the driver's door, on the script's own times.
    def thunk(start, amp):
        i0 = int(start * SR)
        m = int(0.30 * SR)
        if i0 + m >= n:
            return
        tau = np.arange(m) / SR
        body = np.sin(2 * np.pi * 70 * tau) * np.exp(-tau * 14) + 0.5 * np.sin(2 * np.pi * 140 * tau) * np.exp(-tau * 20)
        click = bandpass(rng.standard_normal(m), 900, 5000) * np.exp(-tau * 70)
        mix[i0:i0 + m] += amp * (body * 0.8 + click * 0.5)
    thunk(tl['enter'] + 0.15, 0.35)   # the door opens (the latch)
    thunk(tl['drive'] - 0.25, 0.55)   # and shuts behind the driver
    thunk(tl['exit'] + 0.2, 0.45)     # the wreck's door is pushed open
    thunk(tl['exit'] + 2.9, 0.40)

    # --- Engine and tires, from the vehicle's rpm and speed.
    on = interp_track(tt, driving * (tt < crash).astype(float), n)
    on = np.minimum(on, 1.0)
    rpm_t = interp_track(tt, rpm, n)
    spd = np.abs(interp_track(tt, speed, n))
    thr_t = np.clip(interp_track(tt, thr, n), -1, 1)
    f0 = rpm_t / 60.0 * 2.0   # a four-cylinder fires twice a revolution
    phase = np.cumsum(f0) / SR
    eng = np.zeros(n)
    for h, a in ((1, 1.0), (2, 0.55), (3, 0.38), (4, 0.25), (6, 0.18), (8, 0.12)):
        eng += a * np.sin(2 * np.pi * h * phase)
    eng *= 0.5 + 0.5 * np.sin(2 * np.pi * phase * 0.5) * 0.0 + 0.0
    intake = lowpass(rng.standard_normal(n), 900) * (0.25 + 0.75 * np.clip(thr_t, 0, 1))
    eng_load = 0.55 + 0.45 * np.clip(thr_t, 0, 1)
    mix += on * (eng * 0.07 * eng_load + intake * 0.10)
    tire = bandpass(rng.standard_normal(n), 300, 2500) * np.clip(spd / 25.0, 0, 1) ** 1.5
    mix += on * tire * 0.13

    # --- The crash: the recorded impact event sets when and how hard.
    for e in impacts:
        ti, sp, en = float(e['time']), float(e['speed']), float(e['energy'])
        if ti >= total:
            continue
        i0 = int(ti * SR)
        m = int(3.2 * SR)
        m = min(m, n - i0)
        tau = np.arange(m) / SR
        k = min(1.0, sp / 22.0)
        boom = np.sin(2 * np.pi * (48 + 20 * np.exp(-tau * 9)) * tau) * np.exp(-tau * 5.5)
        crunch = bandpass(rng.standard_normal(m), 150, 4500) * np.exp(-tau * 7.0)
        metal = sum(np.sin(2 * np.pi * f * tau + rng.random() * 6) * np.exp(-tau * dcy) for f, dcy in ((412, 6), (887, 8), (1530, 11), (2310, 13), (3270, 17)))
        glass = bandpass(rng.standard_normal(m), 3000, 9000) * np.exp(-tau * 9) * (0.4 + 0.6 * (tau < 0.4))
        hiss = bandpass(rng.standard_normal(m), 2500, 8000) * np.exp(-tau * 0.9) * np.clip(tau / 0.3, 0, 1) * 0.25
        mix[i0:i0 + m] += k * (boom * 0.95 + crunch * 0.8 + metal * 0.035 + glass * 0.28) + hiss * 0.07
        # debris pattering down
        for j in range(26):
            s = ti + rng.uniform(0.15, 2.8)
            j0 = int(s * SR)
            mm = int(0.05 * SR)
            if j0 + mm < n:
                tau2 = np.arange(mm) / SR
                mix[j0:j0 + mm] += bandpass(rng.standard_normal(mm), 600, 5000) * np.exp(-tau2 * 70) * rng.uniform(0.05, 0.25) * k

    # --- Score: a slow generated pad. Chords change every 16 s; it swells for the drive, thins into the sunset and the night.
    chords = [(57, 60, 64), (53, 57, 60), (55, 59, 62), (52, 55, 59), (57, 60, 64), (50, 53, 57), (52, 55, 59), (57, 61, 64), (45, 52, 57)]
    pad = np.zeros(n)
    for idx, ch in enumerate(chords):
        s0, s1 = idx * 16.0, idx * 16.0 + 20.0
        i0, i1 = int(s0 * SR), min(int(s1 * SR), n)
        if i0 >= n:
            break
        seg_t = np.arange(i1 - i0) / SR
        env = np.clip(seg_t / 4.0, 0, 1) * np.clip((20.0 - seg_t) / 4.0, 0, 1)
        for note in ch:
            f = 440.0 * 2 ** ((note - 69) / 12.0)
            for det in (-0.004, 0.0, 0.004):
                pad[i0:i1] += env * (np.sin(2 * np.pi * f * (1 + det) * seg_t) + 0.35 * np.sin(2 * np.pi * 2 * f * (1 + det) * seg_t))
    pad = lowpass(pad, 1400)
    level = np.interp(t, [0, 8, 30, 47, 60, 88, 91, 93, 100, 125, 140, 142], [0.0, 0.05, 0.07, 0.10, 0.14, 0.14, 0.02, 0.05, 0.07, 0.08, 0.05, 0.0])
    # a soft pulse that carries the drive
    pulse_env = np.interp(t, [46, 52, 88, 91], [0, 1, 1, 0])
    pulse = np.sin(2 * np.pi * 55 * t) * np.exp(-((t * 2.0) % 1.0) * 6.0) * pulse_env
    mix += pad * level * 0.5 + pulse * 0.10

    # fade in/out, normalise with a soft limiter
    mix *= np.clip(t / 1.5, 0, 1) * np.clip((total - t) / 3.0, 0, 1)
    mix = np.tanh(mix * 1.2) / np.tanh(1.2)
    mix /= max(1e-9, np.max(np.abs(mix))) / 0.89
    left = mix
    right = np.roll(mix, 37)  # a hair of stereo width
    pcm = (np.stack([left, right], axis=1) * 32767).astype('<i2')
    import wave
    with wave.open(out, 'wb') as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print('wrote', out, f'{total:.1f} s, {len(steps)} footsteps, {len(impacts)} impacts')


main()
