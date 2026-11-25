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
