# Professional audio for SAIGE

The brief: recorded sound first, generated sound only to fill gaps, and an engine that plays it from real simulation state. Showcase 03's soundtrack is synthesised from the simulation trace
because no recordings were reachable when it was made; it is a placeholder for exactly the layers described here, not the intended quality.

## Runtime technology: evaluation

| Option | Licence | Fit | Verdict |
| --- | --- | --- | --- |
| **miniaudio** 0.11.26 | public domain / MIT-0 (one header, no dependencies) | device I/O on Windows/Linux/macOS/Web; decoders for WAV, FLAC, MP3 (Vorbis and Opus through optional extras); `ma_engine` with 3D positioning, attenuation models, doppler, cones; a node graph (low/high-pass, shelf, delay, splitter, and an extras reverb node); an **offline, device-less mode** so audio can be rendered deterministically and tested | **recommended mixer and device layer** |
| Steam Audio | Apache-2.0 | HRTF binaural, geometry-based occlusion and sound propagation, reflections baked or real-time; C API | optional later adapter on top of the mixer for HRTF and propagation; not needed first |
| OpenAL Soft | LGPL-2.1 | mature 3D mixing, EFX reverb | LGPL linking obligations on a statically distributed engine; not preferred |
| SoLoud | zlib | simple, small, basic 3D | less control over the graph than miniaudio |
| FMOD Studio / Wwise | proprietary, free tiers with revenue limits | the industry authoring tools | vendor SDKs would sit behind a capability-gated adapter if ever wanted; not a dependency of the core |
| libopus + opusfile, stb_vorbis, dr_libs | BSD / public domain | codecs for streaming music and dialogue | add when the first Opus asset ships |

**Proof of concept** (`tools/production/audio_poc`, builds in seconds, needs only `raw.githubusercontent.com`): miniaudio rendered offline, with SAIGE's axes mapped to its (+X right, +Y up, -Z forward) in one
adapter function. `check_poc.py` measures what it rendered and passes: a 40 m/s pass-by 8 m off the road axis gives a Doppler shift of 497.8 Hz approaching and 394.0 Hz receding (physics: 497.9 and 394.1), the
level peaks at closest approach, a source on the left is louder in the left channel, an occlusion low-pass (20 kHz down to 300 Hz plus a gain drop) removes the tone, the reverb node leaves a tail, and the RPM layer blend
is equal-power at every rpm. This is a feasibility proof for Codex's runtime, not the runtime: ownership, threading and streaming of the real audio system are Codex's design (shared interfaces are proposed on issue #43).

**Prototype of the engine's own model** (`tools/production/audio_poc/audio_proto.cpp`, `build_proto.sh`, `check_proto.py`; AUD-1): the real `engine_audio` and `impact_sound` from `include/saige/audio_model.hpp` drive miniaudio voices offline,
and the rendered WAVs are measured. Measured on this machine (CPU, offline render; nothing here says anything about a device or latency): across a 13 s idle-to-redline-and-back sweep the engine's fundamental follows the smoothed rpm to
1.2% (worst over 120 windows), and the level dips at most 2.6 dB below its neighbours at the loop crossfades (the stand-in loops are not level matched; recorded ones must be, in `audio_prep`); the impact peak level tracks the model's gain
within 15%, a heavier blow plays lower (centroid 5.5 kHz to 4.9 kHz over 100 J to 80 kJ), the category follows the materials (car on asphalt metal, on grass soft, on gravel granular, a rock on ice glassy, on concrete hard) and selects the sound
played, and a 20 J scrape is silent. Seven mutations of the mapping (pitch, gains, volume, category, metal flag, raw rpm) are each caught.
**The sounds in it are procedural stand-ins** (harmonic stacks, decaying partials, noise grains): they exist so the mapping can be measured. No recording was acquired: the brief prefers professionally recorded, appropriately licensed
assets, and acquiring them is an owner action (a licence and, for anything paid, a budget). Candidates to evaluate when that is decided, none verified or fetched: CC0 engine and impact material on OpenGameArt and Freesound (licence per asset, attribution
records in `docs/assets/licenses.md`), royalty-free professional libraries (Sonniss GameAudioGDC bundles, whose licence text must be read before use) and a commissioned recording session for the vehicle loops (4 to 6 fixed-rpm loops, on and off throttle).
**Dependency status (updated 2026-11-25):** miniaudio 0.11.26 (public domain / MIT-0, one header) is approved as the production audio backend behind SAIGE's own `AudioOutput`; see `docs/audio.md` for the runtime, licences and evidence. This page keeps the original evaluation
(device I/O and decoders are not exercised here: only the device-less engine).

## What the runtime has to do, and how the mixer covers it

| Requirement | Approach |
| --- | --- |
| 3D spatial audio, distance attenuation | `ma_sound` spatialisation, inverse-distance with per-class min/max distances; mono sources for 3D voices |
| Doppler | source and listener velocities from the simulation each tick (the PoC verifies the numbers) |
| Obstruction and occlusion | the engine raycasts `StaticCollision` from listener to source; the hit fraction drives a per-voice low-pass cutoff and gain (smoothed over about 100 ms); material absorption later |
| Environment reverb | reverb node on an aux bus; zones (tunnel, interior, open) set room size, damping and wet level, cross-faded on entry |
| Seamless loops | `prep/audio_prep.py --loop` builds an equal-power crossfade and reports the seam; the mixer loops the decoded buffer sample-exactly |
| Randomised variation | banks of 4-8 takes per event; no immediate repeats; pitch ±3-5 %, gain ±2 dB; round-robin for footsteps |
| RPM-dependent vehicle audio | 4-6 on-load and off-load loops recorded at fixed rpm, equal-power blend in log-rpm and re-pitched to the actual rpm (`rpm_blend` in the PoC), plus turbo, gear whine, exhaust pops, gear-shift and tyre layers driven by throttle, load, slip and speed |
| Material-dependent impacts | event carries the two surface materials, impact speed and energy; a material-pair table selects the bank (metal-on-concrete, glass, plastic, body panel) and the energy selects intensity layers |
| Surface-dependent footsteps | the foot-contact event carries the surface under the foot (a raycast already identifies the primitive; surface ids come from the world's materials), speed selects walk/run/sprint banks |
| Weather-dependent ambience | ambience beds (wind, rain light/heavy, thunder one-shots with distance delay) cross-fade on weather state; interior filtering via occlusion |
| Dynamic mixing | buses: master, vehicles, sfx, ambience, dialogue, music, ui; ducking (dialogue ducks music and ambience), a final limiter, loudness target about -16 LUFS for the game mix |
| Dialogue | streamed Opus, subtitle events, priority queue, ducking |
| Cinematic audio | the same engine rendered **offline** (device-less) against the capture clock so a video's soundtrack is the real mix driven by the same simulation events |

Events come from simulation and engine state (`VehicleImpact`, wheel slip, gait, door phase, weather), never from the camera or the video script.

## Sources, by sound class

Reachability and terms are in `integration-report.md`. Nothing recorded has been acquired yet: Freesound and ElevenLabs need keys, Sonniss and Pixabay need a browser download.

| Class | First choice | Also |
| --- | --- | --- |
| Engine idle, acceleration, deceleration, exhaust, gears, transmission | Freesound CC0 recordings (search `car engine idle`, `engine rev`, `gear shift`; keep clean on-load/off-load takes), Sonniss vehicle libraries | ElevenLabs to fill missing layers |
| Tyre friction, skids, surface roll | Freesound CC0, Sonniss | |
| Collisions, metal deformation, glass, debris | Sonniss (the strongest free set), Freesound CC0 | OpenGameArt CC0 impact packs |
| Footsteps by material, movement, clothing | Freesound CC0 foley, Kenney audio (CC0), Sonniss foley | |
| Doors and interactions | Kenney audio, Freesound CC0 | |
| Rain, thunder, wind, wildlife, ocean, urban traffic, interior and environmental ambience | Freesound CC0 field recordings (check the loop quality), Sonniss ambience | |
| Emergency vehicles | Freesound CC0 sirens | |
| Voices and dialogue | ElevenLabs (original voices) after the owner decides on a paid plan for commercial rights | |

Licence rules for audio: CC0 and CC-BY (with credit) may enter the public repository, but we re-fetch rather than commit. Sonniss and Pixabay files are `embedded-only`: use them inside the game, never as standalone
files in a public repository; Sonniss's recent bundles forbid use as AI training data (processing a sound for the game is not training). ElevenLabs output on the free plan is non-commercial and needs attribution:
fine for prototypes, not for a shipped game until a paid plan is chosen by the owner.

## Preparation (`tools/production/prep/audio_prep.py`)

Trim silence, resample (48 kHz), integrated-loudness normalisation (BS.1770) or peak normalisation for one-shots, seamless loops with a measured seam, fades, mono for 3D voices, Opus/Vorbis/FLAC encode, metadata JSON
(`saige-assets` stores duration, rate, channels, peak, LUFS, silence ratio and loop seam in the record). Tested here on the showcase's own audio: a loop seam of 0.0008 and -22.9 LUFS against a -23 target.
