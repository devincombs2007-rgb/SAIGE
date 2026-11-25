# Vehicle dynamics options (differential, engine braking, slip-angle tires, damage)

`include/saige/vehicle.hpp`, `src/vehicle.cpp`. Everything here is opt-in: with the options at their defaults the simulation is bit-identical to what it was before they existed (checked by the tests, and by the unchanged Showcase 03 replay
checks). Numbers below are from the tests in `tests/vehicle_drivetrain_test.cpp` and `tests/vehicle_damage_test.cpp` (software build, CPU, no performance claim).

## Differential (`VehicleConfig::differential`)

The drive force is shared between the driven wheels against the friction limit each wheel had on the previous substep (published as `VehicleWheelState::drive_force`).

| Mode | Behavior | Ice left, asphalt right, 3 s of full throttle |
| --- | --- | --- |
| `Legacy` (default) | equal share, each wheel clipped by its own friction circle | 9.5 m/s |
| `Open` | every wheel gets the same force, so the weakest wheel caps both (an air-borne wheel starves the other) | 1.4 m/s, 383 N at each wheel |
| `LimitedSlip` | a wheel may carry up to `lsd_bias` times the weakest wheel plus `lsd_preload` | 3.2 m/s |
| `Locked` | shared in proportion to each wheel's grip | 8.8 m/s |

On even grip an open differential accelerates like the original model (16.1 m/s after 3 s in both). No wheel-speed state is modelled, so a locked differential does not bind in a corner and there is no wheel spin: traction is the friction circle.

## Engine braking (`VehiclePowertrain::engine_brake`)

Drag torque at the redline (N m), scaled with engine speed, through the gear and final drive and the efficiency, over the wheel radius. Only with the throttle released, fading out below a walking pace. The force matches that formula to 3%; the deceleration it adds is 0.76 m/s^2 in third at 28 m/s and 1.76 m/s^2 in first at 12 m/s for 120 N m.

## Slip-angle lateral tire (`slip_angle_lateral`, `lateral_stiffness`, `lateral_slide_ratio`)

`F = limit * sin(C atan(B a))`, `tire_lateral_ratio()`. It rises with stiffness B, peaks near 11 degrees (B = 12), and falls to `lateral_slide_ratio` of the peak for a sliding tire; it never exceeds what would cancel the sideslip in one substep. Measured:

* A controlled drift: steer in at 22 m/s with full power, then a feedback steer and a lifted throttle: 31 degrees of body slip held above 12 degrees for 2.7 s, caught, 99 degrees of heading change. The original model reaches 3 degrees under the same inputs. Uncaught, the same slide turns the car around.
* Coasting corners agree with the original model to 10% short of the limit and hold 0.85 of its limit grip at it (the slide ratio). Under full power the driven wheels spend grip on drive force, so the corner is wider than the original model's (which gave lateral force priority).
* No ringing: a steering pulse at 30 m/s dies away in 4 s without a sign change; creeping at 1.5 m/s with the stiffest tire is as smooth as the original model (this is what the cap on the lateral force is for).

## Persistent damage (`VehicleConfig::damage`, `Vehicle::damage()`)

A state, not a mesh: numbers in [0, 1] for six zones, four wheels, four suspensions and the engine, fed by the energy a collision removes from the chassis (the impact events' `energy`) over `energy_capacity` (150 kJ: the energy of a 1450 kg car at 14 m/s), attributed by where the contact point is on the
body. A head-on hit at 4, 8, 12 m/s damages the front 0.07, 0.30, 0.67 (the collision loss is 95% of the kinetic energy). A corner hit hurts the near wheel and suspension, not the far one; only a front hit hurts the engine; a landing hurts the underbody; a spin-out into a rail hurts the side that hit.

Consequences (physical, and nothing else): a crumpled zone rebounds less (restitution falls by up to `restitution_loss`: a destroyed front rebounds at under 0.45 of a new one), a hurt wheel grips less (stopping 1.5 times further) and toes outward (the car pulls toward the bent wheel's side), a hurt suspension sags
(more compression under the same weight), a hurt engine accelerates slower (4.0 s to 20 m/s becomes 9.9 s). A rubber barrier gives back more and takes less out of the car than concrete (less damage) at 5, 9 and 13 m/s.

**No geometry changes.** The chassis stays the same collision box. A host that draws damage must say it is a visual driven by this state and must not present it as the shape changing physically.

Save format (`serialize_damage`, `deserialize_damage`): 92 bytes, "SVDM", version 1, sizes, little-endian IEEE-754 numbers, a 32-bit FNV-1a checksum. The reader refuses a wrong magic, a newer version, a different size, a bad checksum and any value outside [0, 1] (every single-bit flip and every truncation is refused in the test); a version 2 reader must read version 1 or migrate it explicitly.

## What is not done

No tire slip-ratio or wheel-speed state; no mesh deformation; a locked differential does not bind; engine braking has no clutch model; damage has no repair cost or tow; no vehicle-vehicle contact.
