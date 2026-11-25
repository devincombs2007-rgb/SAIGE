#!/usr/bin/env python3
"""Showcase 04 "A World Alive": the soundtrack, mixed from what the simulation did.

The film's renderer writes one line per frame (the listener's position, every vehicle with its speed and siren, the residents within earshot with their speeds, and the world's impact events). This script turns
that log into a stereo track: every sound is placed in the world at its source and heard from the camera (distance attenuation, equal-power panning, Doppler on moving sources, a muffling filter indoors).

Sources:
  recorded (CC0, OpenGameArt: domasx2 engine loop, qubodup / Fantozzi footsteps, 75 CC0 breaking/falling/hit SFX, rubberduck/jute-dh weather): engines (pitched by speed), footsteps (one per stride walked),
  impacts (chosen by the energy the engine reported), wind.
  procedural (generated here, no recording was available from the reachable sources): the sirens (wail and yelp), tyre and road noise, the hazard relay tick, the sea (filtered noise with a swell), the city's
  distant hum and a quiet evolving pad under the evening.
Nothing in the mix is placed by hand: if the log has no impact, there is no impact sound.

usage: alive_mix.py LOG.jsonl OUT.wav [--assets DIR_OF_OPENGAMEART_CACHE] [--seed N]
"""
import argparse
import json
import math
import os
import subprocess
import sys

import numpy as np

SR = 48000
FPS = 30.0
SPF = int(round(SR / FPS))   # samples per frame
C_SOUND = 343.0


def load(path, cache):
    """Decode any audio file to mono float32 at SR (through ffmpeg, cached as raw f32)."""
    name = os.path.splitext(os.path.basename(path))[0] + "_%08x.f32" % (abs(hash(path)) & 0xffffffff)
    out = os.path.join(cache, name)
    if not os.path.exists(out):
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", path, "-ac", "1", "-ar", str(SR), "-f", "f32le", out], check=True)
    return np.fromfile(out, dtype=np.float32)


def lowpass(x, cutoff, state=None):
    """One-pole low-pass over a block; returns (y, state)."""
    a = math.exp(-2.0 * math.pi * cutoff / SR)
    y = np.empty_like(x)
    s = 0.0 if state is None else state
    # block-wise recursion: vectorised by a cumulative filter (exact for one pole)
    # y[n] = a*y[n-1] + (1-a)*x[n]
    from scipy.signal import lfilter
    y, zf = lfilter([1.0 - a], [1.0, -a], x, zi=[s * a])
    return y.astype(np.float32), float(zf[0] / a if a else 0.0)


def pan_gains(lp, rp):
    """Equal-power pan from a [-1, 1] position (-1 left)."""
    ang = (np.clip(lp, -1, 1) + 1.0) * math.pi / 4.0
    return math.cos(ang), math.sin(ang)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("out")
    ap.add_argument("--assets", default="/tmp/saige-cache/opengameart")
    ap.add_argument("--cache", default="/tmp/audio04")
    ap.add_argument("--seed", type=int, default=3)
    ap.add_argument("--limit-frames", type=int, default=0)
    args = ap.parse_args()
    os.makedirs(args.cache, exist_ok=True)
    rng = np.random.default_rng(args.seed)
    A = args.assets

    engine = load(f"{A}/car_engine_loop_96khz_4s/contents/engine-loop/engine-loop-1-normalized.wav", args.cache)
    engine2 = load(f"{A}/racing_car_engine_sound_loops/loop_2_0.wav", args.cache)
    wind = load(f"{A}/wind/contents/wind/Wind.ogg", args.cache)
    steps_stone = [load(f"{A}/fantozzis_footsteps_grasssand_stone/contents/Fantozzi-footsteps/ogg/Fantozzi-Stone{s}{n}.ogg", args.cache) for n in (1, 2, 3) for s in "LR"]
    hits = [load(f"{A}/75_cc0_breaking_falling_hit_sfx/contents/bfh1_hit_{i}.ogg", args.cache) for i in ("01", "02", "03", "04", "05", "06", "10", "13")]
    metal_hit = load(f"{A}/75_cc0_breaking_falling_hit_sfx/contents/bfh1_metal_hit_01.ogg", args.cache)
    metal_fall = load(f"{A}/75_cc0_breaking_falling_hit_sfx/contents/bfh1_metal_falling_01.ogg", args.cache)
    glass = load(f"{A}/75_cc0_breaking_falling_hit_sfx/contents/bfh1_glass_breaking_05.ogg", args.cache)
    breaking = load(f"{A}/75_cc0_breaking_falling_hit_sfx/contents/bfh1_breaking_03.ogg", args.cache)

    frames = []
    with open(args.log) as f:
        for line in f:
            frames.append(json.loads(line))
            if args.limit_frames and len(frames) >= args.limit_frames:
                break
    n_frames = frames[-1]["f"] - frames[0]["f"] + 1
    f0 = frames[0]["f"]
    total = (n_frames + 1) * SPF + SR * 4
    L = np.zeros(total, np.float32)
    R = np.zeros(total, np.float32)

    # ---- state ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    engine_pos = {}      # vehicle id -> read position (samples, fractional) in the loop
    siren_phase = {}     # vehicle id -> phase (cycles)
    tyre_state = {}
    last_veh = {}        # id -> (x, y, z)
    last_cam = None
    step_dist = {}       # ped id -> distance since the last step
    step_count = {}
    last_ped = {}
    hazard_t = {}
    sea_state = [0.0, 0.0]
    hum_state = [0.0]

    def listener(fr):
        e = np.array(fr["cam"][:3], np.float64)
        look = np.array(fr["cam"][3:], np.float64)
        fwd = look - e
        fwd[2] = 0.0
        n = np.linalg.norm(fwd)
        fwd = fwd / n if n > 1e-6 else np.array([0.0, 1.0, 0.0])
        right = np.array([fwd[1], -fwd[0], 0.0])
        return e, fwd, right

    def spatial(p, e, right, ref=2.5):
        d = np.array(p, np.float64) - e
        dist = float(np.linalg.norm(d))
        gain = ref / max(dist, ref)
        pan = float(np.dot(d / max(dist, 1e-6), right))
        return gain, pan, dist

    def add_block(buf_l, buf_r, start, block, gain, pan):
        gl, gr = pan_gains(pan, pan)
        end = min(start + len(block), len(buf_l))
        n = end - start
        if n <= 0:
            return
        buf_l[start:end] += block[:n] * (gain * gl)
        buf_r[start:end] += block[:n] * (gain * gr)

    def read_loop(loop, pos, rate, n):
        idx = pos + np.arange(n) * rate
        idx = np.mod(idx, len(loop))
        i0 = idx.astype(np.int64)
        fr_ = idx - i0
        i1 = (i0 + 1) % len(loop)
        return loop[i0] * (1 - fr_) + loop[i1] * fr_, pos + n * rate

    def play(sample, start, gain, pan, rate=1.0, maxlen=None):
        s = sample if maxlen is None else sample[:maxlen]
        if rate != 1.0:
            idx = np.arange(0, len(s) - 1, rate)
            s = np.interp(idx, np.arange(len(s)), s).astype(np.float32)
        add_block(L, R, start, s, gain, pan)

    # ---- per frame ----------------------------------------------------------------------------------------------------------------------------------------------------------------
    t_axis = np.arange(SPF) / SR
    for fr in frames:
        fi = fr["f"] - f0
        s0 = fi * SPF
        e, fwd, right = listener(fr)
        indoors = fr.get("in", 0) == 1
        vel_cam = (e - last_cam) * FPS if last_cam is not None else np.zeros(3)
        last_cam = e.copy()
        # vehicles
        seen = set()
        for v in fr["veh"]:
            vid, kind, x, y, z, speed, rpm, siren, disabled, braking = v
            seen.add(vid)
            pos = (x, y, z + 0.8)
            prev = last_veh.get(vid, pos)
            vvel = (np.array(pos) - np.array(prev)) * FPS
            last_veh[vid] = pos
            gain, pan, dist = spatial(pos, e, right)
            if dist > 140.0:
                continue
            # radial velocity (positive: moving apart) for Doppler
            d = np.array(pos) - e
            dn = d / max(np.linalg.norm(d), 1e-6)
            vr = float(np.dot(vvel - vel_cam, dn))
            dop = C_SOUND / (C_SOUND + vr)
            sp = abs(speed)
            heavy = kind in (2, 4, 5)
            loop = engine if not heavy else engine2
            pitch = (0.50 + 0.036 * min(sp, 22.0)) * (0.80 if heavy else 1.0) * dop
            amp = (0.20 + 0.45 * min(sp / 12.0, 1.0)) * (1.35 if heavy else 1.0) * (0.35 if disabled else 1.0)
            blk, engine_pos[vid] = read_loop(loop, engine_pos.get(vid, float(rng.integers(0, len(loop)))), pitch, SPF)
            add_block(L, R, s0, blk.astype(np.float32) * 0.32 * amp, gain, pan)
            # tyres and road: noise rising with speed
            if sp > 2.5:
                nz = rng.standard_normal(SPF).astype(np.float32)
                nz, tyre_state[vid] = lowpass(nz, 700.0 + 60.0 * sp, tyre_state.get(vid))
                add_block(L, R, s0, nz * (0.012 * min(sp, 20.0) / 20.0) * (sp / 12.0), gain, pan)
            # siren
            if siren:
                ph = siren_phase.get(vid, 0.0)
                tt = fr["t"] + t_axis
                if kind == 4:   # ambulance: wail
                    f_inst = 760.0 + 420.0 * (0.5 + 0.5 * np.sin(2 * math.pi * 0.42 * tt))
                else:           # fire engine: yelp
                    f_inst = 700.0 + 520.0 * (0.5 + 0.5 * np.sin(2 * math.pi * 1.6 * tt))
                f_inst = f_inst * dop
                phase = ph + np.cumsum(f_inst) / SR
                siren_phase[vid] = float(phase[-1] % 1.0)
                w = np.tanh(2.6 * np.sin(2 * math.pi * phase)) * 0.5 + 0.18 * np.sin(2 * math.pi * 2 * phase)
                add_block(L, R, s0, w.astype(np.float32) * 0.22, gain * 1.2, pan)
            # hazards tick
            if disabled and dist < 40.0:
                ht = hazard_t.get(vid, 0.0) + 1.0 / FPS
                if ht >= 1.0 / 1.6 / 2.0:
                    ht = 0.0
                    tick = np.zeros(300, np.float32)
                    tick[:60] = np.sin(2 * math.pi * 2200.0 * np.arange(60) / SR) * np.exp(-np.arange(60) / 14.0)
                    add_block(L, R, s0 + int(rng.integers(0, SPF // 2)), tick * 0.25, gain, pan)
                hazard_t[vid] = ht
        for vid in list(last_veh):
            if vid not in seen:
                del last_veh[vid]
        # footsteps
        for p in fr["ped"]:
            pid, x, y, z, speed, state, inside = p
            if speed < 0.35:
                step_dist[pid] = 0.0
                continue
            pos = (x, y, z + 0.1)
            gain, pan, dist = spatial(pos, e, right, ref=2.0)
            if dist > 45.0:
                continue
            step_dist[pid] = step_dist.get(pid, 0.0) + speed / FPS
            stride = 0.62 + 0.07 * min(speed, 3.0)
            while step_dist[pid] >= stride:
                step_dist[pid] -= stride
                k = step_count.get(pid, 0)
                step_count[pid] = k + 1
                smp = steps_stone[(pid * 7 + k) % len(steps_stone)]
                loud = 0.55 + 0.25 * min(speed / 3.0, 1.0)
                play(smp, s0 + int(rng.integers(0, SPF)), gain * loud * 0.7, pan, rate=1.0 + 0.04 * float(rng.standard_normal()))
        # impacts: the engine's own report
        for ev in fr["ev"]:
            typ, x, y, energy, vid, kind = ev
            gain, pan, dist = spatial((x, y, 0.5), e, right, ref=4.0)
            if typ == 0 and energy > 800:
                big = energy > 25000
                body = hits[int(abs(energy)) % len(hits)] if not big else metal_hit
                play(body, s0, min(1.0, 0.25 + energy / 90000.0) * gain * 1.4, pan)
                if energy > 6000:
                    play(metal_fall, s0 + 600, 0.5 * gain, pan, maxlen=int(SR * 1.8))
                if big:
                    play(breaking, s0, 0.8 * gain, pan, maxlen=int(SR * 2.0))
                    # a low thump: a decaying sine
                    nn = int(SR * 0.9)
                    tt = np.arange(nn) / SR
                    thump = (np.sin(2 * math.pi * (60.0 - 20.0 * tt) * tt) * np.exp(-tt * 5.0)).astype(np.float32)
                    add_block(L, R, s0, thump * 0.9, gain * 1.2, pan)
            if typ == 1:   # the car is wrecked: glass goes
                play(glass, s0 + 1200, 0.7 * gain, pan, maxlen=int(SR * 2.5))

    # ---- beds ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    T = len(L)
    # the sea: low-passed noise with a slow swell, louder near the water (camera latitude from the log)
    cam_y = np.array([fr["cam"][1] for fr in frames])
    sea_amp_frames = 0.020 + 0.20 * np.exp(-np.maximum(0.0, 78.0 - cam_y) / 55.0)
    sea_amp = np.interp(np.arange(T) / SPF, np.arange(len(frames)), sea_amp_frames)
    tt = np.arange(T) / SR
    sea = rng.standard_normal(T).astype(np.float32)
    from scipy.signal import lfilter
    a = math.exp(-2 * math.pi * 500.0 / SR)
    sea = lfilter([1 - a], [1, -a], sea).astype(np.float32)
    swell = 0.55 + 0.45 * np.sin(2 * math.pi * (0.09 * tt + 0.7 * np.sin(2 * math.pi * 0.013 * tt)))
    sea *= (swell * sea_amp).astype(np.float32) * 2.2
    L += sea * 0.9
    R += np.roll(sea, 1900) * 0.9
    # distant city: a low hum of far traffic
    hum = rng.standard_normal(T).astype(np.float32)
    a = math.exp(-2 * math.pi * 260.0 / SR)
    hum = lfilter([1 - a], [1, -a], hum).astype(np.float32) * 0.40
    L += hum * 0.5
    R += np.roll(hum, 777) * 0.5
    # wind (recorded), quiet, stronger at height
    wl = np.resize(wind, T) * (0.035 + 0.05 * np.clip(np.interp(np.arange(T) / SPF, np.arange(len(frames)), [fr["cam"][2] for fr in frames]) / 40.0, 0, 1)).astype(np.float32)
    L += wl
    R += np.roll(wl, 2113)
    # a quiet evolving pad under the evening: A minor 9, slow chord changes; procedural
    chords = [[110.0, 164.81, 220.0, 329.63], [87.31, 130.81, 174.61, 261.63], [98.0, 146.83, 196.0, 293.66], [110.0, 164.81, 246.94, 329.63]]
    pad = np.zeros(T, np.float32)
    seg = max(1, int(T / (len(chords) * 3)))
    for k in range(len(chords) * 3):
        ch = chords[k % len(chords)]
        a0, a1 = k * seg, min(T, (k + 1) * seg + SR * 4)
        env = np.ones(a1 - a0, np.float32)
        fade = min(len(env) // 2, SR * 4)
        env[:fade] = np.linspace(0, 1, fade)
        env[-fade:] = np.minimum(env[-fade:], np.linspace(1, 0, fade))
        t_local = np.arange(a1 - a0) / SR
        for fq in ch:
            pad[a0:a1] += (np.sin(2 * math.pi * fq * t_local) + 0.3 * np.sin(2 * math.pi * fq * 2.003 * t_local)).astype(np.float32) * env * 0.004
    # the pad swells as the evening deepens (louder in the second half)
    pad *= (0.5 + 0.9 * np.clip(tt / (T / SR), 0, 1)).astype(np.float32)
    L += pad
    R += np.roll(pad, 1500)

    # ---- indoors: a muffling filter on the frames where the camera is inside a room ----------------------------------------------------------------------------------------------------
    ind = np.array([fr.get("in", 0) for fr in frames], np.float32)
    ind_s = np.interp(np.arange(T) / SPF, np.arange(len(frames)), ind).astype(np.float32)
    if ind_s.max() > 0.5:
        Ll, _ = lowpass(L, 1400.0)
        Rl, _ = lowpass(R, 1400.0)
        L = (L * (1 - ind_s) + Ll * ind_s * 0.8).astype(np.float32)
        R = (R * (1 - ind_s) + Rl * ind_s * 0.8).astype(np.float32)

    # ---- master: soft limiter, fades, normalise to about -3 dBFS peak ---------------------------------------------------------------------------------------------------------------------
    n_out = n_frames * SPF
    L, R = L[:n_out], R[:n_out]
    fade_in, fade_out = int(SR * 1.2), int(SR * 2.0)
    env = np.ones(n_out, np.float32)
    env[:fade_in] = np.linspace(0, 1, fade_in)
    env[-fade_out:] = np.linspace(1, 0, fade_out)
    L *= env
    R *= env
    peak = max(np.abs(L).max(), np.abs(R).max(), 1e-6)
    g = 0.7 / peak
    L = np.tanh(L * g * 1.1) / 1.0
    R = np.tanh(R * g * 1.1) / 1.0
    out = np.stack([L, R], axis=1)
    pcm = (np.clip(out, -1.0, 1.0) * 32767.0).astype("<i2")
    import wave
    with wave.open(args.out, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(pcm.tobytes())
    print(f"wrote {args.out}: {n_out / SR:.1f} s, peak before master {peak:.3f}")


if __name__ == "__main__":
    sys.exit(main())
