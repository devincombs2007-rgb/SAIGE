# Showcase 03 defects: root causes, fixes and guards

The delivered film is unchanged (build 9c87457). The three defects the owner reported were investigated by measurement, fixed in code, and guarded by tests that fail against the defect. The app now
contains the fixes, so a future showcase does not inherit them. Branch `claude/production-infrastructure`.

## D1. Window pops, white and dark windows

Measured, not guessed: a fixed camera on the main street, a sweep of the film's clock, and the pixel value of a lit window.

| Moment (film time) | Lit-window pixel before | After the next frame (0.5 s later) |
| --- | --- | --- |
| about 9.3 s (sun crosses 6 degrees, sunrise) | 159 | 65 |
| about 118.8 s (sun crosses 6 degrees, dusk) | 106 | 178 |
| about 123.8 s (1 degree) | 186 | 243 |
| about 128.8 s (-4 degrees) | 248 | 255 (clipped to white) |

Root causes:

1. The app chose one of four pre-scaled material sets by sun elevation. Every lit surface changed in one frame at three elevations: the pop.
2. The emission at the top level (3.2, 2.5, 1.4) under the film's night exposure (about +1.7 EV) clipped to pure white, losing the warm color: the white windows.

Also observed, not changed: the unlit ("dark") windows are a mirror-like dark glass (metal 0.6) that reads near black against the lit ones; they are consistent from frame to frame.

Refuted: depth fighting between the window quad and the wall (the renderer is reverse-Z with a 32-bit float depth; the resolution at 200 m is about 0.01 mm against a 30 mm offset), and camera-driven flicker
(a 24-step camera sweep at night shows a smooth, camera-dependent window count with no frame-to-frame spikes).

Fix: `SceneLighting::emissive_gain`, a per-frame global multiplier of emission in the model shader (default 1, so nothing else changes), documented in `docs/rendering/dynamic-lighting.md`; the app has one
material set and ramps the gain continuously with `emissive_gain_at(elevation)` (`time_of_day.hpp`); the window emission was lowered so it keeps its warm hue (0.30, 0.185, 0.075). After the fix the largest
change of a lit window between frames 0.5 s apart is 15 levels (was 94).

Guards: `horizon_time_of_day` (bound on the per-frame step at 30 and 60 fps, monotone evening ramp, and a control that the old four-level switch fails the same bound); the Vulkan test "SceneLighting::emissive_gain
scales emission continuously" (mutation-checked: removing the shader multiply fails 16 checks); the validation checks in `test_scene_renderer`.

Still open (not part of this defect): lit and dark windows are still flat quads with no interior and no point-light contribution; see the backlog.

## D2. Wheels wobble while turning

Root causes, both measured on the real model (`horizon_wheel_rig` prints them):

1. CarConcept's front wheel nodes have about 30 degrees of steer baked into their matrices (axle 30.2 and 30.0 degrees off the car's +Y), the rears about 0.7 degrees, and every wheel an arbitrary spin. The app
   spun the wheels about the car's +Y, so each front wheel swung on a tilted axle. (The old pivot, taken from group bounds, was within 2 mm of the true center and was not the cause.)
2. The app applied `rot_y(-spin)` where the vehicle's spin is positive for forward travel; a right-handed rotation about +Y carries the top of the wheel forward for a positive angle, so the wheels rolled backward.

Fix: `wheel_rig.hpp` measures each wheel from its vertices (the axle is the direction in which the wheel is thin, the center is the middle of the circle it sweeps), re-poses it to center-at-origin and
axle-on-+Y, and `wheel_spin` gives the forward-positive roll. Residual axle error after re-posing: 0.000 degrees.

Guards: `horizon_wheel_rig` (synthetic wheels with baked steer, spin and offset, the real model's four wheels, the tire radius against the vehicle's 0.384 m, a control that fails for an un-posed wheel, and the roll
direction). Mutation-checked both ways.

Known limit: at 30 frames per second a fast wheel turns about 100 degrees per frame, so the picture can show the wheel-of-a-wagon effect. That is aliasing of a faithful simulation, not a bug; the remedy for a
future film is temporal supersampling (average of sub-frames), tracked in the backlog.

## D3. Character walks through the roadside barriers

Root cause: the collision hull of each barrier was authored 90 degrees off the model (the model's long axis is the engine's Y; the hull's was X), and several drawn props (lamps, trees, benches) had no hull at all.

Fix: one prop table (`props.hpp`) holds model orientation, scale, visual bounds and hull for every prop; the renderer and the collision builder both read it. Guards: `horizon_sim` ("what is drawn is what collides":
hull invariants, rays across every barrier, a walking capsule against real geometry), `horizon_props` (the table against the importer's bounds of the real models). Mutation-checked.
