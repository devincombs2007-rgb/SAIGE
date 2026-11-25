# Audio runtime (AUD-2)

**Decision (owner and development director, 2026-11-25):** miniaudio is SAIGE's production audio backend, behind a SAIGE-owned interface, subject to licence verification and integration testing. Both are done for the first runtime slice and recorded below.

## Layers

| Layer | Where | Depends on |
| --- | --- | --- |
| Decisions from simulation state: engine rpm loops, impact category, footsteps, tires, water, ambience bed, reverb for a space | `include/saige/audio_model.hpp`, `src/audio_model.cpp` | nothing (pure, deterministic) |
| Mixer: voices, spatial placement, doppler, occlusion, reverb, buses, ducking, voice limits | `include/saige/audio.hpp` (`AudioMixer`), `src/audio.cpp` | nothing: renders offline, no device |
| Device runtime: the mixer driven on a device thread | `AudioRuntime` in the same files | an `AudioOutput` |
| Device and decoders | `engine/audio_miniaudio` (`make_miniaudio_output`, `decode_audio_file`) | miniaudio 0.11.26 + stb_vorbis 1.22, optional |

Game code includes only `saige/audio.hpp` and `saige/audio_model.hpp`; replacing miniaudio means writing another `AudioOutput` and decoder, nothing else. The mixer is SAIGE's own on purpose: it is what the tests measure, it renders the same bits on every platform and every block size, and a cinematic render and a game use the same code.

## Licences (verified from the pinned sources, 2026-11-25)

| Component | Version | Licence | Notes |
| --- | --- | --- | --- |
| miniaudio | 0.11.26 | dual: public domain (Unlicense) or MIT No Attribution, at our choice | `miniaudio.h`, SHA-256 `6b376c7f...`; no attribution required |
| stb_vorbis | 1.22 (the copy in miniaudio's `extras/`) | dual: public domain or MIT | Ogg Vorbis decoding; SHA-256 `4c7cb2ff...` |

Both are fetched by `tools/audio/fetch_miniaudio.py DIR` from the tag `0.11.26` on raw.githubusercontent.com and verified against pinned hashes (a mismatch aborts); nothing is committed. A version change is a reviewed edit of the pin. No other dependency is added: the reverb (Freeverb, public domain algorithm), the filters and the resampler are SAIGE code.

## What the mixer does

* **Voices.** Mono clips, loop or one-shot, linear-interpolated resampling by pitch and by clip rate, generation-checked ids (a finished or stolen voice's id is dead for every call), a voice limit (64) with stealing: the least important voice goes (lowest priority, then quietest, then oldest), the newcomer replaces it only if it is more important, otherwise it is refused and counted.
* **Spatial.** Listener position, forward, up and velocity. Distance gain by inverse, linear or exponential model with minimum and maximum distances and a rolloff factor. Equal-power panning on the listener's right axis, softened inside the minimum distance. Doppler from source and listener velocities along the line between them (pitch limited to half an octave either way). Air absorption (lowpass falling with distance) and occlusion (0..1 from the world: a gain loss plus a 12 dB/octave lowpass towards 800 Hz). Non-spatial voices are centred (UI, music, a cabin sound).
* **Reverb.** One shared Freeverb-style reverb, per-voice sends, room size, damping and wet level from `reverb_for_space(enclosure, volume, hardness)`.
* **Buses.** Effects, Vehicle, Ambience, Footsteps, Water, Music, Dialogue, each with a gain; ducking rules (a source bus above a threshold pulls a target bus down with attack and release); a master gain; clipping is counted and the output clamped (no limiter yet: see Next).
* **Determinism.** The same clips and the same calls give the same bits, and splitting `render()` calls at any sizes changes nothing (the mixer works in chunks of 64 frames; parameters take effect at chunk boundaries and ramp across the chunk, so a moving source or a gain change does not click).

## Models: the simulation decides, the mixer plays

`engine_audio` (three loops pitched by rpm, equal-power crossfade, throttle load, smoothed rpm), `impact_sound` (material category, gain by energy, pitch by energy), `footstep_sound` (speed, weight, landing, slide, surface), `tire_sound` (rolling noise by speed and surface, skid by slip), `water_sound` (splash by entry energy, flow by speed and immersion), `ambience_mix` (time of day, wind, rain, built-up land, enclosure), `reverb_for_space`. All validate input and are deterministic; the numbers are thresholds the tests pin and the mutation checks protect, not tuned-by-ear values: tuning by ear needs the owner's ears on a device.

## Threading

`AudioMixer` is not thread-safe. `AudioRuntime` owns a mixer and a device and hands out `access()` (a held mutex plus the mixer). The device thread uses `try_lock`: if game code is inside a control section the callback plays that block as silence and counts it (`skipped_blocks()`), so game code can never stall the device thread. Measured on miniaudio's null backend with the game thread taking the lock about 3,000 times a second: 0 of 62 blocks skipped (5 of 57 under ThreadSanitizer, which slows everything). A lock-free command queue is the next step if profiling on a real device shows skips.

## Evidence

| Check | Result |
| --- | --- |
| `audio_test` (attenuation, panning, doppler, occlusion, reverb, ducking and buses, voices, determinism, validation) | passes; 15 of 15 first mutations killed after five tests were strengthened (survivors had exposed untested rolloff factor, priority-vs-loudness stealing, duck smoothing, gain ramping and clip counting) |
| `audio_model_test` (footsteps, tires, water, ambience, reverb) | passes; 12 of 12 mutations killed |
| `audio_device_test` (WAV decode, mono downmix, bad input, `AudioRuntime` on the null backend, stop/restart, no callback after stop) | passes; ThreadSanitizer clean; two mutations (callback always skips, stop does nothing) killed |
| `saige_audio_scene` with real recordings | 20 s, 40 voices, no clipping, no rejected voices, bit-identical across two renders, engine zero-crossing rate 88 -> 147 per second as revs climb from idle |

CI: the mixer and model tests run in the default build and tests; `.github/workflows/audio.yml` builds the adapter and runs everything on Linux under ASan+UBSan and ThreadSanitizer and on Windows with MSVC `/W4 /WX` on SAIGE's code. These are software and null-device results on shared runners.

**Hardware checks that need a person and a device (not done):** that a real device opens at 48 kHz on Windows (WASAPI), latency at a 10 ms period, no underruns while rendering, and whether the mix sounds right. The adapter returns a message instead of crashing when no device opens.

## Recordings

Production objective: professional recordings from legitimate sources. Done: the asset catalogue holds 11 candidate packs from OpenGameArt (nine CC0, two CC BY 3.0 needing attribution, one with an unknown licence not used): engines, footsteps on stone, sand, snow and gravel, impacts, breaking glass, doors, rain, wind and thunder. They are fetched into the asset cache by `saige-assets` and are never committed; `saige_audio_scene` plays eight of them through the mixer. The attribution strings are in the catalogue records (`assets/catalog/audio`) and travel with any release.

Not done and why: Freesound is unreachable from this environment's network policy and needs an API key (not authorised); Kenney's host and Sonniss return 403 to the proxy; Sonniss's bundles are browser downloads with an embedded-use licence. Missing classes: tire squeal and surface roll, water splashes and flow, car doors and cabin, a proper engine set with measured rpm (the racing loops are used with an autocorrelation estimate of their firing frequency, an assumption to replace with recorded rpm), footsteps on grass and wood, emergency sirens. The owner can supply Freesound or Sonniss downloads; no spending or key is needed from SAIGE's side beyond that.

## Next

Occlusion from the world (ray casts from `StaticCollision`) driving `set_voice_occlusion`; reverb zones that cross-fade on entry; a lookahead limiter and loudness target; per-bank round-robin and no-repeat selection; streaming for long ambience and music (Opus/Vorbis from disk instead of fully decoded clips); a lock-free command queue if device profiling asks for it; recording acquisition per class above; hearing the result on a Windows workstation.
