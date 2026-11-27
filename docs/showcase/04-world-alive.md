# Showcase 04: "A World Alive"

A coastal district that runs by itself, drawn by the real SAIGE renderer: sixty residents live on daily schedules (walking, crossing at signals, sitting in cafes, going home), cars drive a signalled
street grid on the engine's raycast-suspension vehicle, one car is distracted and crashes, an ambulance and a fire engine are dispatched through preempted signals, and the district goes back to its
evening. Nobody scripts the action: the film is a camera placed on whatever the simulation produced. Every pixel is read back from the renderer's output target. This page says what is real, what is
approximated, what is not in the film, and how to reproduce it.

Owner of the work: Claude Code (graphics, district, living-world integration, film). Core owns the engine services it consumes. Coordination: issue #43. Branch `claude/showcase-04`.

## What the film shows, and what drives it

| Layer | Implementation | Status |
| --- | --- | --- |
| District | `src/district*.cpp`: a seed-deterministic grid (three avenues, three streets, quay, promenade, pier) with 113 buildings, furnished interiors with stairs, 473 props, 200 pedestrian nodes and 754 collision boxes; `build_district(seed)` changes buildings, props and the cafe position | real, procedural, tested (`alive_district_test`) |
| Residents | `src/world.cpp`: `world_sim::WorldSimulation` schedules and venues; `crowd::CrowdSolver` proposes velocities at 20 Hz; each resident is a `CapsuleCharacter`, which is authoritative for position; crosswalks wait for the walk phase of the signal controller | real, tested (`alive_world_test`: no overlap, none stuck) |
| Traffic | `src/traffic_driver.cpp`: every moving car is a `saige::Vehicle` on `StaticCollision`; `traffic::TrafficSimulation` runs with external physics and the host reads lane and distance back from the chassis; connector lanes are Bezier curves with pure-pursuit steering; signals | real, tested (`alive_traffic_test`) |
| Accident and response | one stochastic distraction event per run, the car becomes a wreck and raises an `Accident` event, an ambulance and a fire engine are dispatched with signal preemption and are `Cleared` after 25 s on scene; residents react | real events from the simulation; **the crumple is visual** (a scripted deformation of the body mesh driven by the impact event), not a deformable body |
| Animation | UAL humanoid (Quaternius, CC0), GPU skinning; `LocomotionSet` / `LocomotionController` pose each resident from its real speed | real |
| Water | sea mesh sampled from the engine's `WaterSurface` (`harbor_sea_config`) | real surface; **boats are decorative** (not `FloatingBody` simulated) |
| Rendering | PBR metallic-roughness, HDR, three-cascade sun shadows, atmosphere with `render::solar_position` for the date and time, stars, AO, bloom, FXAA, GPU skinning, emissive windows and lamps ramped with the evening | real, see docs/rendering |
| Lighting at night | emissive windows, lamps, headlights and sirens with bloom | **no point lights**: lights do not illuminate their surroundings |
| Camera | `render/director.cpp`: shots are chosen from the report of a headless scout pass (`alive_scout`), which finds the time the simulation produced its accident; follow shots attach to a real resident or vehicle present at that time; the camera never moves a simulated object | real, no scripted action |
| Audio | `tools/alive_mix.py` builds a stereo track from the **per-frame audio log the simulation wrote** (engines by vehicle speed, footsteps by resident speed, sirens, impact, ambience), with panning, distance attenuation, Doppler and an indoor low-pass | recorded CC0 sounds where reachable; stereo mix computed offline, **not** a runtime audio engine |
| Not in the film | Jolt-backed destruction and ragdoll (needs the optional Jolt adapter, not part of this build), boat buoyancy, point lights, image-based lighting, a real-time run | disclosed on the end card |

## Assets and licences

Fetched into a local cache (not committed; records under `assets/catalog/`).

* Poly Haven PBR textures and props: CC0-1.0.
* Universal Animation Library (Quaternius): CC0.
* Car Concept, (c) Khronos Group, created by Eric Chadwick of Darmstadt Graphics Group GmbH, 2024, CC BY 4.0; paint variants are tints applied by the renderer.
* Sound effects: CC0 packs from OpenGameArt.
* Cesium ion and Cesium 3D Tiles data were **not** used.
* No game content or proprietary creative material is in this repository or the film.

The attributions are on the end card.

## Reproducing

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSAIGE_ENABLE_VULKAN=ON -DSAIGE_BUILD_GRAPHICS=ON && cmake --build build
build/showcase/alive_district_test      # district invariants
build/showcase/alive_world_test 150     # residents: no overlap, none stuck
build/showcase/alive_traffic_test       # traffic, the accident, the response
build/showcase/alive_scout 8 230 > scout8.txt    # when does seed 8 produce its accident (the director reads this)
build/showcase/saige_alive --film --seed 8 --film-seconds 180 --preroll 25 --fps 30 --time-scale 47 \
    --scout-file scout8.txt --width 1920 --height 1080 --shadow-map 4096 --crf 14 \
    --start-frame 0 --end-frame 300 --video seg_0000.mp4           # repeat per 300-frame chunk; chunks are exact because frames before the range are simulated, not drawn
build/showcase/saige_alive --film ... --no-render --audio-log audio8.jsonl
python3 -I showcase/04-world-alive/tools/alive_mix.py audio8.jsonl mix8.wav --seed 8
showcase/04-world-alive/tools/assemble.sh SEGMENT_DIR mix8.wav film.mp4 1920 1080 30
```

`saige_alive --frames a,b,c` renders single frames of the film (stills).

## Frame rate, resolution and hardware

The film is an **offline capture**: one frame at a time on a **software Vulkan rasterizer (llvmpipe, 4 CPU cores)**, 1920x1080 at **30 distinct frames per second**, 180 s. It makes no real-time
claim, and nothing here is physical-GPU evidence. Measured cost on this machine is about 5 s per frame at 1080p with a 4096 shadow map.

## The delivered film

| Item | Value |
| --- | --- |
| File | `SAIGE_A_World_Alive.mp4` (not committed: 206 MB; the segments, audio log and mix are reproducible from this branch) |
| Container, video | H.264 High, yuv420p, 1920x1080, 30 fps constant, `+faststart` (iPhone compatible), crf 20 |
| Length | 191.0 s = 180.0 s of film (5400 frames, 18 segments of 300 frames) + an 11 s end card; 5730 frames counted by `ffprobe -count_frames` |
| Audio | AAC-LC 192 kb/s, 48 kHz stereo, integrated loudness -16.6 LUFS, true peak -1.0 dBFS (the encoder overshoots the -1.5 dB target by 0.5 dB; the pre-encode mix is -28.6 LUFS before normalisation) |
| SHA-256 | `00dcf1fdd2fc33c7535d2d743fc81a7ea46f10aa1d66201f020a958c6ee3e312` |
| Seed and time | seed 8, `--time-scale 47` (the film runs from late afternoon through sunset to night) |
| Where the accident is | about 1:35 into the film (the 95 s frame shows the smoking wreck); the scout pass found it and the director cut to it; responders and the night follow |

The first assembly had an end-card line wider than the frame; it was shortened and the film re-assembled (only `assemble.sh` changed; no frame was re-rendered). The 5400 film frames are the original
render, unmodified.

## Feature and technology inventory

Every item below is exercised in the film. "Tested" means a test in the repository covers it; nothing here is a measurement on physical GPU hardware.

* Renderer: Vulkan RHI, PBR metallic-roughness model pipeline, HDR target, three-cascade sun shadows (4096 map), atmosphere sky with a solar position for the date and time, procedural stars, screen-space AO, bloom, FXAA, GPU skinning, emissive windows and lamps ramped with the evening.
* Simulation (CPU, deterministic from the seed): district generator, 60 residents with schedules and venue visits, crowd solver, capsule characters, traffic with signals, raycast-suspension vehicles on static collision, one stochastic accident, emergency dispatch with signal preemption, residents reacting to the impact.
* Animation: UAL humanoid, locomotion state machine and blend space posed from each resident's real speed.
* Water: the engine's wave surface sampled for the sea mesh.
* Camera: scout pass + director (shots follow real residents and vehicles; the camera never moves a simulated object).
* Audio: offline stereo mix from the simulation's per-frame audio log (engine tone by vehicle speed, footsteps by resident speed, sirens, impact, ambience), with panning, distance attenuation, Doppler and an indoor low-pass, then loudness normalisation.
* Film tooling: chunked, resumable rendering (frames before a chunk are simulated but not drawn, so chunks are exact), an ffmpeg pipe, an assembly script (concatenate, title, end card, loudnorm, H.264/AAC).

## Validation

Run on the film's binary (`build-vk`, a **Debug** build; the film was rendered with it, not a Release build) on 2026-11-26:

| Check | Result |
| --- | --- |
| `alive_district_test` | district ok: 113 buildings, 473 props, 200 pedestrian nodes, 3703 interior cells, 754 collision boxes |
| `alive_world_test 150` | simulated 150 s; closest pair of centres 0.685 m; worst overlap 0; worst stuck 0; 46 arrivals (3 into interiors); 39 crossings; world ok |
| `alive_traffic_test` | accident at +0.8 s, cleared at +65.8 s; 6 impacts; 2 responders dispatched and on scene; 6 resident reactions; 0 red-light runs; max lane error 0.29 m; traffic ok |
| Film frames | 5400 frames, all 1920x1080, in 18 segments; assembled file probed with `ffprobe` (see the table above); a contact sheet of ten frames across the film and the title, accident and end-card frames were inspected by eye |
| Hosted CI, `12c73d7` | Linux ASan/UBSan, Linux lavapipe, Physics Jolt and Audio passed; **the Windows MSVC build failed** because `popen`/`pclose` are not MSVC names |
| Hosted CI, `d2f94c6` | the same fix passed on the engineering branch (`50bd467`: Graphics, Physics, Audio all green); for this branch it is pending until pushed and run |

## Commit ids

| Commit | What |
| --- | --- |
| `72d959e`, `50dce61`, `c1e8141` | the living world, traffic, the renderer app |
| `12c73d7` | scout pass, director, film mode, spatial audio mix |
| `50ca1f3` | far skirt to 300 km, light fog, assembly script, first draft of this page. **The film's engine and assets are exactly this commit** (binary built 2026-11-25, not rebuilt during the render) |
| `d2f94c6` | portable ffmpeg pipe for MSVC (cherry-pick of `50bd467`); Windows-only, does not change any pixel or sound on Linux |
| (this commit) | the end-card text fix in `assemble.sh` and this page |

## Known limitations

* **Software rendering.** The film is an offline capture on llvmpipe at roughly 5 s per frame. It says nothing about real-time or GPU performance, and it makes no 60 FPS claim.
* **Lighting.** There are no point lights, no image-based lighting and no global illumination: night scenes are lit by the sun-and-sky model plus emissive surfaces and bloom, so lamps do not light their surroundings.
* **Crumple is visual.** The accident's deformation is a scripted mesh change driven by the impact event; the vehicle is not a soft body. Jolt destruction and ragdoll are not in this build.
* **Boats are decorative**; boat buoyancy is not simulated in the film.
* **Vegetation and weather.** Trees are simple low-poly meshes with no wind; there is no weather. (Engine-side CPU modules for wind, weather and vegetation exist on a separate branch and are not in the film.)
* **Audio** is an offline mix from the event log with CC0 recordings, not a runtime audio engine; some sound classes are approximated where no recording was reachable.
* **Characters** share one rig and one mannequin mesh split into clothing regions (tinted per resident).
* **Resolution of the horizon and far field** is limited by the land skirt (300 km) and uniform fog; the earlier seam was fixed by a restart of the render, not by post-processing.
* **Debug build.** The film binary is a Debug build. A Release build was not used or compared, so the reproduction command above (Release) is untested against this film.

## Reproduction notes

The cache of third-party assets (Poly Haven, UAL, CarConcept, OpenGameArt sounds) is fetched, not committed. The Python mix needs `python3 -I` and the default `--assets` cache path. The assembly step needs `ffmpeg`
with `libx264`, `drawtext` and the DejaVu fonts. At about 5 s per frame, the 5400 frames are roughly 7.5 hours of rendering on 4 cores (derived from the measured per-frame cost, not a separate timing of the whole run); the assembly took about 6 minutes.
