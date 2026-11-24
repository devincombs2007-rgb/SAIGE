# Physical surface materials

A tire, a body or a falling object meets different things depending on what it touches. `include/saige/physics_material.hpp` gives the collision world a material per primitive without changing
`StaticCollision` (which stays geometry only).

* `Surface` (default, asphalt, wet asphalt, concrete, gravel, dirt, grass, sand, snow, ice) and `PhysicsMaterial` (tire grip, rolling resistance, body friction, restitution, density). The numbers are textbook ranges for
  dry conditions relative to dry asphalt (`VehicleConfig::tire_friction` remains the dry-asphalt value of the tire); they are approximations for a believable difference between surfaces, and a game replaces the table.
* `SurfaceMap`: one `Surface` per box and per triangle, indexed by the primitive ids the collision reports. A primitive the map does not cover reads as `Default`, so a stale map cannot crash a query.
* Combining: friction by the geometric mean, restitution by the larger; both symmetric. `Rubber` (grippy, lively) exists so that the restitution side of the rule has a material above the vehicle body's own.
* `Vehicle::step(input, dt, world, scratch, const SurfaceMap* surfaces = nullptr)`: the contact surface's `tire_grip` scales the tire's friction circle and its `rolling_resistance` the rolling term; and the body's collision with a primitive combines the body's `body_friction` and `body_restitution` with that primitive's material. With no map the
  result is bit-identical to before (`vehicle_surface` checks this against an all-Default and an all-Asphalt map, and the existing `vehicle` test is unchanged).

Threading and ownership: the map is read during the call only and is not retained; share it read-only between threads. The vehicle is the only consumer so far (character friction and debris restitution are next; see the backlog).

Evidence (`vehicle_surface` test, software build, Debug): from about 14 m/s the car stops in 11.6 m on asphalt, 12.9 m wet, 14.3 m gravel, 16.0 m grass, 17.5 m sand, 34.4 m snow and 83.5 m on ice (where friction
limits the stop it matches v^2 / (2 mu g) within 15 to 30 percent); braking from 15 m/s with grip on one side and ice on the other turns the car 7.2 degrees toward the grippy side (mirrored cases agree; even ground
turns it 1e-9 degrees); five seconds of full throttle reach 22.7 m/s on asphalt, 15.7 on grass and 2.5 on ice; coasting for five seconds loses 0.86, 2.63 and 3.97 m/s on asphalt, grass and sand. A head-on crash at 14 m/s into a wall bounces the car back at 8.2 m/s from a rubber wall against 2.1 m/s from concrete and 1.9 m/s with no map; an angled crash (a front corner first) spins it at 1.91 rad/s against rubber, 1.65 against concrete and 1.64 against ice (the spin comes mostly from the normal impulse at the corner, so friction changes it little). Each effect was
shown to disappear when its term is removed from the vehicle (mutation).
