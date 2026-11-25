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
