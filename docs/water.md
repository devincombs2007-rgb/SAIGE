# Water (`include/saige/water.hpp`, `src/water.cpp`)

A wave surface with height, normal, flow and foam queries, and floating bodies that respond to it. Independent of rendering and of the vehicle. Tests: `tests/water_test.cpp` (software build, no performance claim).

## The surface

A sum of Gerstner waves (direction, wavelength, amplitude, phase) over a mean level. Each wave's speed follows the dispersion relation of the configured depth, `omega^2 = g k tanh(k d)` (deep water when the depth is 0): a 50 m wave travels at 8.8 m/s in deep water and slower in 5 m of water; a long wave in 1 m of water at sqrt(g d) to 3%. The
crest of the surface moves at that speed to 1% in the test. `choppiness` pulls points toward the crests; validation refuses a configuration whose `choppiness * sum(k A)` could fold the surface (0.95).

| Query | What |
| --- | --- |
| `height(x, y, t)` | exact (the horizontal displacement is inverted by fixed-point iteration), mean over a wavelength equals the level |
| `normal(x, y, t)` | analytic; equals the slope of the height to 4e-10 in the test |
| `velocity(x, y, z, t)` | the orbital flow of every wave (A omega, decaying as exp(-k depth) in deep water, linear wave theory in finite depth: flat orbits, no vertical motion at the bed) plus a uniform current |
| `jacobian`, `foam` | the Jacobian of the horizontal displacement (1 calm, near 0 where a crest is about to fold, never negative for a valid configuration) and a foam amount from it |
| `waves_from_wind(speed, direction)` | a deterministic Pierson-Moskowitz sea state: significant wave height 0.53, 2.11, 4.78 m for 5, 10, 15 m/s against the textbook 0.54, 2.14, 4.82 |

Everything is stateless and deterministic (same configuration and time give the same bits; there is no random source).

## Floating bodies

A rigid box filled with sample cells. Each cell feels buoyancy in proportion to its submersion at its own position (so a body follows the waves, and a long one averages them), quadratic drag against the water's flow there (a current carries a body, a moving body slows), and gravity acts on the whole.
Heave and roll/pitch radiation damping stand in for the energy a hull gives to the waves. Substeps of at most 1/240 s whatever `dt` is asked.

Measured:

* Floating draft equals the density ratio (Archimedes) to 0.01 in fresh and sea water for densities 250 to 950; a denser body sinks monotonically and reaches the terminal speed of the drag balance within 5%; a body of the water's density hangs where it is; a light body released under water rises to its draft.
* A small buoy heaves with the wave amplitude (0.409 m on a 0.40 m wave), in phase; a 50 m barge on 20 m waves heaves 0.118 m.
* Drag follows the analytic law `dv/dt = -k (v + u0) v` to 0.3% over 6 s.
* The settled draft is 0.500 at every step from 1/15 to 1/240 s; the heave amplitude in waves agrees to 0.03%; steps of a full second stay finite.
* A current carries the body to its speed within 3% in either direction without changing its draft; a cube twice the size takes twice as long.
* Stability: a wide flat box heeled 25 degrees rights itself; a cube of density 0.25 (negative metacentric height) falls over; lowering its center of mass 0.4 m makes it stable.
* Exposed for later systems: `wetted()` (cell position, velocity, submersion), `submerged_volume`, `waterline`. Dropped from 3 m a body reaches the water when free fall says (0.73 s), plunges and settles to its draft.

## Extension points (designed for, not built)

* Wakes and ripples: a host adds a height term from `wetted()` (moving wet cells), not a change to the body.
* Swimming and displacement: a character capsule can use `height`, `velocity` and the same cell drag; the displaced volume is exposed.
* Coastal waves: the depth already enters the dispersion; shoaling (amplitude growth and refraction over a depth map) would take a depth function in place of the constant.
* Weather: `waves_from_wind` sets the sea state; a time-varying wind is a host that replaces the wave set (interpolating amplitudes).
* Foam: the Jacobian-based amount is ready for a renderer to consume.

## Limits

No added mass, no wave diffraction or radiation beyond the damping ratios, no slamming or planing, no body-body contact, one rigid box per body, one uniform current, a heightfield surface (no breaking waves, spray or interior flow).

## Many floating bodies (WATER-2)

`tools/benchmarks/water_workloads.cpp` (`saige_water_workloads [--cost-only|--accuracy-only]`) places N bodies at deterministic scattered positions in a 300 x 300 m wind sea (twelve Gerstner waves, Hs about 2 m) with
distance bands from the viewer, steps them at 60 Hz, and compares each option with the exact reference: 216 sample cells per body, every wave, serial. Release build, one CPU of this development container (shared, not a
workstation; absolute numbers will differ, the ratios are the finding). Cost is milliseconds per 1/60 s frame (median of three runs of 60 frames):

| Variant | 1 body | 10 | 50 | 100 |
| --- | --- | --- | --- | --- |
| reference: exact, 216 cells, serial | 1.005 | 9.587 | 49.082 | 104.857 |
| shared surface cache (`WaterFieldCache`) | 1.507 | 15.144 | 77.267 | 142.742 |
| distance: fewer waves (8, then 4 of 12) | 0.391 | 5.622 | 25.627 | 48.524 |
| distance: fewer cells (6, 4, 3 per axis) | 0.133 | 2.593 | 10.146 | 21.007 |
| cache + fewer waves + fewer cells | 0.606 | 7.965 | 34.483 | 63.624 |
| all of that on 4 threads | 0.453 | 7.680 | 35.728 | 65.913 |
| exact reference on 4 threads | 1.056 | 4.099 | 16.720 | 32.650 |

Accuracy against the reference (20 bodies for 20 s; RMS error of the body's height and roll; "near" is within 40 m, "far" beyond):

| Variant | near z (mm) | near roll (deg) | far z (mm) | far roll (deg) |
| --- | --- | --- | --- | --- |
| shared surface cache | 1.49 | 0.019 | 2.45 | 0.032 |
| fewer waves (8 and 4 of 12) | 0.00 | 0.000 | **295.7** | **2.881** |
| fewer cells (6, 4, 3) | 0.00 | 0.000 | 0.64 | 0.181 |
| exact on 4 threads | 0.00 | 0.000 | 0.00 | 0.000 |

(Zero near error is by construction: bodies within 40 m run the reference configuration. Nothing nearby is traded for a benchmark number.)

What the measurements say, including what did not work:

* **Threads** (`step_bodies`): 3.2x at 100 bodies on 4 threads with no change in the result (bit-identical for 1, 4 and 7 threads, tested). Not worth it for one body (thread start-up outweighs 1 ms), worth it from about ten.
* **Fewer cells by distance** (`water_lod_cells`): 5x cheaper for bodies beyond 40 m, with 0.6 mm height and 0.18 degree roll error. This is the best trade and costs nothing nearby.
* **Fewer waves** (`WaterSurface::reduced`): 2x cheaper but the far bodies' height error is 0.3 m RMS and roll 2.9 degrees in this sea, because dropping waves removes real motion (the largest four carry 83% of the energy here, not all). Acceptable only for decorative or very distant bodies; not recommended as a default. `filtered(min_wavelength)` removed nothing from this sea (all waves exceed 9 m), so wavelength filtering is of no use for it.
* **Shared surface cache** (`WaterFieldCache`): **slower** than the exact reference for scattered bodies (1.4 times the cost at 100) because bodies do not overlap, so there is nothing to share, and each tile computes 16 points of which a body uses a few. It also approximates the flow below the surface with one wavenumber: 0% at the surface, 4% at 0.25 m under it, 8% at 0.5 m, 14% at 0.9 m, 39% at 3 m, so it refuses (falls back to exact) deeper than `max_flow_depth` (0.25 m by default). It is kept as an opt-in for dense clusters (a fleet of buoys, debris) and cheap height-only queries; it is not used by default and is not a general speed-up. This was a hypothesis that the measurements did not support.
* **Inversion tolerance**: the exact height iterates a fixed point to 1e-8; relaxing it was not tried because the heights are already dominated by other costs (the wave sums), not iteration count.

Recommended defaults for many bodies: `water_lod_cells(distance, full)` per body, `step_bodies` on the available cores (determinism does not depend on the count), the exact surface for everyone. Reduced waves only beyond a distance where error of a third of a meter is invisible.

Not done: wakes, swimming (a character in water), displacement, and two-way coupling with the dynamics backend's rigid bodies (a dynamic crate floating through `FloatingBody` forces). The water module and the dynamics backend are separate; the force evaluation (`FloatingBody` cells) can be applied to a `DynamicsWorld` body as external forces once the dynamics interface has force application (it has impulses; a per-step force API is the next addition).

## Buoyancy on dynamics bodies (`include/saige/buoyancy.hpp`)

`apply_buoyancy(world, body, box, water, t, dt)` applies the water's forces to a box body of the `DynamicsWorld` as impulses at the sample cells, once per step before `world.step(dt)`: the same cells, drag law, stability limit and heave and roll damping as
`FloatingBody`, on a body that also collides, stacks and is pushed by everything else. The world's gravity must be the water's. Measured against the water module's own integrator and Archimedes (Jolt 5.7.0, Linux, 120 Hz, `buoyancy_test`):
floating boxes of density 250, 600 and 900 settle within 4 mm of the draft (submerged volume within 0.4% of mass over water density, residual heave under 3 cm/s: the cell grid makes the volume step); a box of density 1500 sinks to 17.38 m after 8 s
against 17.53 m for `FloatingBody` (same terminal speed, 2.305 against 2.304 m/s); a buoy on an 80 m wave heaves 0.409 m on both with a 1 mm RMS height difference over 30 s; a wide flat box heeled 25 degrees rights itself; a drop from 3 m is wet
at the free-fall time and settles at the draft; a current of (2, 1) m/s carries the body to (1.995, 0.997) m/s; a 3 rad/s spin underwater falls to 0.73 rad/s in 3 s (3.0 with rotational drag off); a 40 m/s body never rebounds; with heave
and roll damping the bobbing and rocking die away (0.004 m and 0.01 degrees after 8 s and 10 s) against 0.049 m and 1.9 degrees without. Ten mutations of the force law are each caught.
Not done: a body with a centre of mass off its box centre (the box centre is used), shapes other than boxes (a cell grid per shape), wakes and swimming (a character in water), and the two-way coupling of a vessel's own thrust.
