# Showcase 03: "Beyond the Horizon"

A scripted run through a streamed world, drawn by the real SAIGE renderer: a character walks, runs, sprints and jumps to a parked car at sunrise, drives it across the countryside, hits a roadside
barrier, climbs out and walks away as the sun sets, and the film ends under the stars. Nothing is composited from outside footage; every pixel is read back from the renderer's output target. This page
says what is real, what is approximated and what is not implemented, and how to reproduce the film.

Owner of the work while Codex is away: Claude Code (Core, Graphics, physics, gameplay, streaming, assets and video). Coordination: issue #43. Branch `claude/showcase-03-world`.

## What the film shows, and what drives it

| Layer | Implementation | Status |
| --- | --- | --- |
| World | `world.cpp`: a procedural height field, a main street with a small town, a cross street, a gas station and a 1.4 km countryside road, in 64 m cells that are a pure function of their coordinates | real, deterministic |
| Streaming | `sim.cpp`: three-by-three resident cells prepared on worker threads (a needed cell that is late is waited for, so replays are bit-identical), the local origin migrates to the focus cell, entities migrate with it, a parked vehicle is a record when its cell is not resident | real |
| Character | Core's `CapsuleCharacter` (walk, run, sprint, jump, step, slopes) driven by a scripted player that issues inputs only | real |
| Vehicle | `include/saige/vehicle.hpp`: a rigid body on four raycast suspensions, tire friction circle, drivetrain, an oriented-box chassis colliding with the static world with impulse response, impact events | real engine functionality, tested |
| Crash | the car leaves the road at the bend and hits a concrete barrier line; the impact event spawns debris bodies (impulse props, collision with terrain), dust and the damage value | real physics response; **damage is approximated visually** (nose squash, lifted bonnet), not a deformable mesh |
| Animation | CesiumMan's walk clip, GPU skinning; gait follows distance covered; the standing pose is the clip frame with the feet closest together | real skinning; **the asset has a walk clip only** (no run, jump, idle or seated clips) |
| Rendering | PBR metallic-roughness, HDR, three-cascade dynamic sun shadows, single-scattering atmosphere with sun disc, moon and stars, time-of-day lighting, screen-space AO, bloom, FXAA, alpha-blended dust, per-frame exposure | real, see docs/rendering |
| Lighting at night | emissive windows and lamps with bloom, a moonlit ambient floor | **no point lights**: lights do not illuminate their surroundings |
| Audio | `tools/audio_recorded.py` mixes **recorded CC0 sound effects from the asset catalog** (six engine loops cross-faded by the vehicle's rpm, footsteps by distance covered, doors, breaking glass, metal, rock and debris at the recorded impact, wind by speed) driven by the simulation trace, with a generated pad, birds and crickets; `tools/audio.py` is the fully synthesised fallback | recorded where a recording was reachable; stereo, not spatialised |
| Not implemented | pedestrians, a seated driver, image-based lighting in this film (the renderer supports it; its environment is fixed per renderer and the film changes the light continuously), soft-body deformation, sampled audio | disclosed on the end card |

## Assets and licences

Fetched by `tools/fetch_assets.py` (checksums in `assets.json`). They are not committed.

* Poly Haven textures and props: CC0-1.0.
* Sound effects: CC0 packs from OpenGameArt by domasx2 (racing car engine loops), qubodup (door set), Fantozzi (footsteps), rubberduck (breaking, falling, hit), tinyworlds (glass break) and ignasd (wind).
* Car Concept, (c) Khronos Group, created by Eric Chadwick of Darmstadt Graphics Group GmbH, 2024, CC BY 4.0.
* Cesium Man, (c) Cesium, CC BY 4.0.

The attributions are on the end card of the film.

## Reproducing

```
python3 showcase/03-horizon/tools/fetch_assets.py ASSET_DIR
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSAIGE_ENABLE_VULKAN=ON -DSAIGE_BUILD_GRAPHICS=ON && cmake --build build
build/showcase/horizon_sim_test                                   # determinism, streaming, checkpoint, crash
build/showcase/saige_horizon_sim_run --out TRACE_DIR              # headless run, trace.csv / impacts.csv / timeline.csv / checkpoint.json
showcase/03-horizon/tools/render_video.sh build ASSET_DIR OUT 1920 1080 30
python3 showcase/03-horizon/tools/audio_recorded.py TRACE_DIR OUT/audio.wav 142   # needs the OpenGameArt packs: saige-assets fetch opengameart <page> --kind audio
showcase/03-horizon/tools/assemble.sh OUT/video_silent.mp4 OUT/audio.wav OUT/film.mp4 1920 1080 30
```

`saige_horizon --still T [--cam x,y,z,lx,ly,lz,fov]` renders single frames; `--start/--end/--fps` render a time range. Every frame is a pure function of the simulation time and the camera script, so
segments render independently.

## Frame rate, resolution and hardware

The film is an **offline capture**: one frame at a time on a **software Vulkan rasterizer (llvmpipe on 4 CPU cores)**, 1920x1080 at **30 distinct frames per second** (the simulation runs at 60 Hz;
every second tick is drawn). It makes no real-time claim and the 60 FPS verdict of the performance report stays `not_evaluated`. 4K60 was not practical on this hardware: a 4K frame took about
3.5 s, so a 4K60 film (8,520 frames) would take over eight hours here; 1080p30 took about an hour and a half. Frame cost is dominated by geometry (about 7,000 draws a frame, three times for the shadow
cascades), not by pixels. Nothing here was measured on a physical GPU.

## Checkpoint

`Sim::checkpoint_json()` stores poses, the script's phase and clock, the damage and the physical props (a save game), not velocities, so it is taken where the world is calm; `horizon_sim_test`
resumes one at 36 s and checks that the resumed run follows the continuous one. A checkpoint taken mid-drive resumes the vehicle at rest.

## Build record and known defects of the delivered film

The delivered film was rendered by the app at commit 9c87457 on software Vulkan (lavapipe, CPU only; no real-time or 60 FPS claim) and is unchanged. Three visible defects in it (wheels wobbling
and turning the wrong way, the character walking through the roadside barriers, and the windows popping and clipping to white at dusk) were root-caused and fixed in the app and engine afterwards; the
measurements, causes, fixes and tests are in [docs/engineering/showcase-03-defects.md](../engineering/showcase-03-defects.md). The fixes are not in the film.
