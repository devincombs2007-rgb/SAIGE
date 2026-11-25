# Capsule character controller

`include/saige/capsule_character.hpp` is a deterministic, fixed-step, capsule-based character controller for the first playable course
(issue #62). It is independent of `CharacterMotor` (#65) and built only on `capsule_sweep.hpp`. It takes the same kind of static box
geometry (as `Aabb`, the layout of `StaticBox`) and the same fixed step, so a host can run either one behind the same input and the
two can be compared tick for tick.

## Per-tick behavior

1. **Recovery.** A capsule found overlapping a box is pushed out the shortest way (exact, `capsule_box_push_out`).
2. **Input.** Horizontal velocity accelerates toward the walk or sprint target, faster on the ground than in the air.
3. **Jump.** On the rising edge of the button, while grounded. Holding the button does not repeat; there is no double jump.
4. **Gravity** while airborne, clamped to the terminal speed.
5. **Collide and slide**, up to four contacts per tick. Each contact moves to the surface, backs off by a 0.1 mm skin and removes the
   motion into the surface (from both the remaining displacement and the velocity). A contact is *ground* only if its normal is
   within `max_slope_radians` (default 50 degrees) of +Z.
6. **Step-up** (grounded only): blocked by a steeper contact, the capsule tries raise, move forward, drop. It is a step if it lands on
   walkable ground and the *supporting surface* is no more than `step_height` (0.35 m) above where it started.
7. **Ground snap**: while grounded, the ground within `ground_snap` (0.12 m) below the feet is followed, so small drops and step edges
   do not make the character airborne.

`teleport` places the character at rest (a vehicle exit, say) and resolves any overlap; `replace_static_geometry` swaps the collision
between ticks and refuses a set that would overlap the character or the reset position, leaving everything unchanged.

## Determinism

No clock, no randomness, no allocation after construction (except `replace_static_geometry`), no dependence on the render rate. The same
inputs in the same build give bit-identical states tick for tick. Results can differ between compilers or flags that change float
contraction (for example FMA), as for any float simulation; compare replays within one build. `tests/capsule_character_test.cpp` runs a
5000-tick scripted course on two independent instances and on a `reset()` instance, compares every tick bitwise, and prints a checksum
of the poses.

## Behavior worth knowing

- **Rim contact.** The bottom of the capsule is a hemisphere, so on the edge of a ledge the contact normal tilts with the overhang
  (`sin(tilt) = overhang / radius`). Up to about 0.23 m of overhang (50 degrees) is ground and the character stands; beyond it the
  contact is steeper than the slope limit and the character slides off. This is the slope limit acting on box geometry; real ramps need a
  non-box collider, and the logic carries over because it acts on contact normals.
- **Step-up reach.** A capsule stopped against a vertical face has its bottom sphere one radius from the ledge's edge. To land on top, the
  step-up carries the capsule forward at least `radius * (1 - sin(max_slope)) + 2 cm` (about 8 cm, under two ticks of walking), once, in
  the tick that climbs.
- **Ground snap backs off vertically.** Backing off along a tilted contact normal made a character standing on a rim creep sideways about
  3e-5 m per tick; found by the tests and fixed.
- **Contacts come from the sweep's stop.** The sweeps (`capsule_sweep.hpp`, #69) return `CapsuleHit::stop`: the float feet position nearest
  the exact contact at which the capsule is clear of every box. The controller adopts it directly (plus the 0.1 mm skin on a slide)
  instead of rebuilding `feet + displacement * t` in float, which at coordinates between 128 m and 256 m (1.5e-5 m per float step) can
  round to the inside of a wall. The skin remains as margin for the next sweep's contact tolerance.
- **Supported range.** Positions are chunk-local meters: the capsule must stay within +-256 m on every axis and every sweep within 64 m
  (`max_local_extent`, `max_sweep_length`). The constructor rejects a configuration whose speeds or reach could exceed that, and a spawn
  outside the range; `teleport` throws for a position outside it and changes nothing; a `step` that would carry the capsule across the
  edge throws `std::invalid_argument` and, like every step, has the strong guarantee (state and jump memory unchanged). The host rebases
  larger world coordinates and streams chunks; the controller never answers from a lower-precision sweep.

- **Driven by an action (`CapsuleInput::drive`).** An action that carries the character (a roll, from a clip's root motion) sets the horizontal velocity directly while grounded: `{active, x, y}` in world axes, in m/s, replacing the acceleration toward the input's target for that tick. The step is otherwise unchanged (collide, slide, step up, ground snap), so a drive cannot carry the capsule through geometry; in the air it is ignored, a non-finite value is an invalid input, its magnitude is capped at `terminal_speed`, and an inactive drive (the default, whatever its numbers) leaves every state bit-identical to a controller without the field (`tests/character_drive_test.cpp`). The animation side (`engine/animation/.../action.hpp`) turns a clip's travel into drives and reads back how far the capsule got.

- **Crouching (`CapsuleInput::crouch`).** The capsule comes down to `crouch_height` at once (0 means 60% of `height`, never under the diameter; the feet stay, the head comes down) and the character moves at `crouch_speed` (0 means half the walking speed); sprint and jump are ignored while crouched. Standing up is refused while the standing capsule would overlap geometry, so a character released under a low ceiling stays crouched, and `state().crouched` says so. `reset()` stands the character up; a collision swap checks the spawn with the standing capsule. Without the input nothing changes (`tests/character_crouch_test.cpp`).

## Non-box geometry (triangles)

`CapsuleCharacter` also takes static triangles (`capsule_triangle.hpp`, #73) beside the boxes: ramps, crests and slopes. They are two-sided,
and the slope, step and ground rules act on contact normals, so they apply unchanged. There is still one controller.

- **Constructors.** `(spawn, boxes, config = {})` is the existing form. The mixed form is `(spawn, boxes, triangles, config)` and the
  configuration is *required*: a defaulted one made `CapsuleCharacter({}, {}, {})` ambiguous. `static_assert`s and a compile test pin both.
- **Replacement.** `replace_static_geometry(boxes)` is a complete-set replacement: it clears the triangles. `(boxes, triangles)` replaces
  both. Both refuse (return false, change nothing) a set that overlaps the character or the reset position, and restore the old set and
  state if the ground probe throws.
- **Stops.** A hit's `stop` is clear of its own kind of surface (#69, #73). When a world has both, the controller also judges it against the
  other kind (a ramp meeting a wall) and backs off if needed; that matters at the rounding scale, below the 1e-5 contact tolerance.
- **Recovery** is transactional: a recovery that would cross a thin triangle, or cannot clear the capsule, leaves the feet where they were
  (and `teleport` restores the whole state), so a failed attempt never publishes a half-resolved pose.

## Verification

`tests/capsule_character_test.cpp`: rest without drift or jitter (ten seconds, state compared exactly), free fall time, wall stop and
slide, concave corner, steps (0.30 m and 0.34 m climbed, 0.40 m is a wall, a small drop is walked down), a 2 m ledge is a fall, jump
apex and edge rules, ceiling, rim slope limit (19 degrees holds, 69 degrees slides off), a 5 cm slab at terminal speed, geometry
replacement, teleport and recovery, invalid input, a wedge between two rims, float rounding across 100-245 m, the supported range (rejections and
the strong guarantee of a step), reset, and the bitwise replay with a
non-penetration invariant on every tick. Mutation checks: skin, ledge-height limit, normal-versus-vertical snap backoff, jump repeat,
walkability, recovery, teleport recovery and velocity, geometry-replacement overlap check, jump-edge memory and step-up reach each make a
test fail. A crease rule for two planes had no effect on any scenario, including the wedge, and was removed; plane projection on the
ground and the vertical-velocity reset while grounded are equivalent mutants (no observable difference on box geometry).

## Cost

On the development container's CPU (Linux, GCC -O2, 128 boxes, no broadphase), one step takes a median of 5.3 microseconds, 17 at the
99th percentile and 30 to 49 at the 99.9th; single outliers of 1 to 2 ms vary run to run and are scheduler noise on a shared machine. A
local CPU figure, not a statement about the target hardware. Zones from `perf.hpp` can time it inside the course benchmark.

## Not covered yet

Ramps and non-box colliders, moving platforms, a broadphase, sliding down steep surfaces under input control, coyote time and jump
buffering, vehicles, and streamed collision ownership. Each is a separate slice.
