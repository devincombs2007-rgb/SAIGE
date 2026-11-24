# Static collision: broadphase, ray casts, capsule, oriented-box and wheel queries

`include/saige/static_collision.hpp` is an immutable bounding-volume hierarchy over static boxes (`Aabb`) and triangles (`Triangle`), with the
queries a game needs from it. It is additive and introduces no second movement system: `CapsuleCharacter` keeps its own geometry. A host can
use this to cull what it gives the character, to answer line-of-sight and ground-probe queries, and to pick candidates for other bodies.

## Queries

| Query | Result |
| --- | --- |
| `raycast(origin, direction, max_distance)` | the nearest primitive along the ray: distance, point, a unit normal facing the origin, and which primitive. Boxes are solid (an origin inside hits at distance 0), triangles two-sided. Analytic in double, any finite coordinates. |
| `query_region(aabb, out)` | the primitives whose bounds overlap a region, in ascending id order |
| `sweep_capsule(capsule, displacement)` | the first contact of a swept vertical capsule, with the full `CapsuleHit` (`t`, `stop`, `contact_t`, normal, point) and which primitive |
| `overlaps_capsule(capsule)` | whether the capsule overlaps anything by more than the contact tolerance |
| `sweep_oriented_box(box, displacement)` | the first contact of a translating oriented box (fixed orientation): `contact_t` (exact, double), `t`, a clear `stop`, the contact normal, and which primitive; see below |
| `overlap_oriented_box(box)` / `overlaps_oriented_box(box, out)` | the deepest penetration (normal from the primitive toward the box, depth), or every penetrated primitive in ascending id order |

Capsule queries share the envelope of `capsule_sweep.hpp` (chunk-local, within 256 m per axis, at most 64 m long, otherwise
`std::invalid_argument`, even when the geometry is empty). The geometry itself may extend anywhere.

## Oriented boxes (the vehicle query)

`include/saige/oriented_box.hpp`. An `OrientedBox` is a center, three half extents (at most 64 m) and an orthonormal frame (`axis_x/y/z`, unit length, perpendicular
within 1e-4, any handedness). `make_oriented_box(center, half_extents, yaw)` builds a vehicle heading about +Z (local +X forward, counter-clockwise from above);
the quaternion overload takes any orientation. Everything is exact in double on the float inputs and a pure function of its arguments.

* **Overlap** is a separating-axis test over the 3 + 3 face normals and 9 edge cross products against a box (13 axes against a triangle), exact for convex polyhedra.
  It reports the minimum-translation normal and depth when the box penetrates by more than `box_contact_tolerance` (0.1 mm). `oriented_box_aabb_gap` and
  `oriented_box_triangle_gap` return the signed separation (positive: apart, a lower bound on the distance; zero: touching; negative: minus the depth).
* **Sweep** is the swept separating-axis test for a translation with the orientation fixed: for each axis the interval of times the projections overlap; the contact is the
  latest entry and requires entry <= earliest exit and entry in [0, 1]. It is exact for translation (not stepped, not conservative advance). A turning box is not
  swept: the caller subdivides the motion and re-orients between steps. Rotational sweeps and wheel contacts are a later slice.
* **Ordering**: the earliest `contact_t`; at an exact tie the deeper initial overlap, then the lower primitive id, boxes before triangles (a box wins an exact tie
  against a triangle), the capsule sweeps' rule extended by the depth. The same rule holds in the pair, combined and `StaticCollision` forms.
* **`stop`** is a conservative clear float center on the way to the contact: the signed gap (`oriented_box_*_gap`) to **every** primitive near the path is not negative. It is found by backing off from the
  contact in doubling steps starting at 2^-17 m, so it is a few coordinate steps short of the contact for a head-on approach and farther (the gap to close over the cosine of the approach angle) for a grazing one; it is **not**
  promised to be the latest representable clear position. Do not recompute `center + displacement * t` in float. `t` is its fraction. `stop_clear` reports whether the guarantee holds (see the exception below).
* **Initial overlap**: if the box already penetrates a primitive by more than the tolerance, the hit has `started_overlapping`, `t = 0`, `stop` = the start, `normal` = the
  minimum-translation direction and `penetration` its depth, whatever the displacement. If it merely touches (gap within the tolerance) it is a hit at `t = 0`
  only when the displacement moves into the surface; moving along it or away is no hit, so a box resting on a floor slides over it and drives away from a wall it touches. **Exception to the clear stop:** a t = 0 hit returns the
  start as `stop`; if the box starts penetrating by a sub-tolerance depth (touching, moving in: `penetration` > 0, not `started_overlapping`) or by more, that start is microscopically or really inside, `stop_clear` is false and the caller pushes
  out along `normal` by `penetration`. With several overlapped primitives the deepest is reported.
* **Envelope**: as the capsule's. The box's bounding box at the start and the end must lie within +-256 m on every axis and the displacement is at most 64 m, else
  `std::invalid_argument` (also for empty geometry and for a non-finite value, a non-positive half extent or a skewed frame). An invalid box or triangle throws in **every** function (overlap, gap, pair and combined sweeps, `oriented_box_stop`).
  Primitives may extend anywhere.
* **Snapshot scope**: shape ids are indices into the boxes and triangles the `StaticCollision` was built from, boxes first. They are valid for that immutable snapshot; a
  durable reference is the integer chunk identity plus the local shape id (Core's convention), never the index alone. Queries take a caller-owned `QueryScratch` and
  are const, so several threads may query one snapshot.

`tests/oriented_box_test.cpp` checks the sweep and the overlap depth against an oracle that shares no code with the separating-axis implementation (vertex containment
and edge-pierce tests on closed solids, with sampling and bisection for the first time of contact): 900 random sweeps against boxes and 900 against triangles agree on whether
there is a hit and on the contact time to 2e-6, the stop is clear and within 50 microns of the contact along the normal, pushing out by the reported depth separates the
box and by less does not; 800 `StaticCollision` queries (sweeps, deepest overlaps, all overlaps) equal brute force in every field; analytic edge, corner, ramp, touching
and initial-overlap cases; ties and every validation failure. Fifteen mutants (dropped axes, a flipped normal, the touching rule both ways, no initial-overlap handling,
no stop back-off, a stop clear of only the first primitive, an ignored exit time, a parallel-axis shortcut, the tie rules, the quaternion layout, the envelope) each fail a test.

## Wheel and terrain contacts (the next vehicle slice)

`include/saige/wheel_contact.hpp`: `probe_wheel(world, probe, scratch)` and `probe_wheels(...)`. One probe per wheel: a support segment from `origin` (the wheel center at the top of its travel, in the published chunk-local frame)
along `direction` for `length` meters, swept as a point (`radius` 0) or as a sphere of the wheel's radius (a capsule sweep as tall as it is wide, so it is as exact as the capsule sweeps). The result: the status, the exact
`distance` along the unit direction (and `travel` = distance / length for the suspension), the wheel `center` at contact, the contact `point`, the unit support `normal`, `slope_cos` and the snapshot-scoped shape id. This is geometry
only: Core owns suspension, traction, wheel state and gameplay.

| Status | Meaning |
| --- | --- |
| `Support` | the nearest contact is a surface the wheel can stand on: `dot(normal, up) >= cos(max_slope)` |
| `Blocked` | the nearest contact is one it cannot: too steep, an overhang's underside (its facing normal points away from `up`), a wall |
| `InitialOverlap` | the probe starts touching or inside the world (a ray whose origin is inside a solid box, a sphere already penetrating a primitive): distance 0, the normal is the push-out direction; recover before trusting a support |
| `Miss` | the segment never touches anything (missing ground past a ledge, out of reach, pointing away) |

Rules: the **nearest contact wins whatever it is** (a blocker between the mount and the ground, such as a beam, a steep face or an underside, is `Blocked`, not skipped to find ground behind it); exact ties go to the lower shape id,
boxes before triangles (a box wins an exact tie against a triangle), the rule of every sweep here; boxes are solid and **triangles are two-sided**, the normal always facing the probe's origin side, so a ramp mesh with flipped winding
still supports and the same triangle from below is an underside; the slope limit is a closed bound on `dot(normal, up)` with `up` the chassis' or the world's (a tilted `up` makes a matching ramp flat to the chassis); touching a
surface from outside and moving away or along it is a `Miss` for that surface (a wheel resting on the ground and driving off is not blocked by it). A sphere probe shares the capsule envelope (path inside +-256 m, `length` at
most 64 m); a ray accepts any finite coordinates. Invalid probes throw, and `probe_wheels` validates every probe before answering any.

`tests/wheel_contact_test.cpp`: flat ground (ray and sphere, oblique and normalized directions, out of reach, pointing away), ramps at 10, 30 and 45 degrees and a tilted `up`, the slope limit (60 degrees blocked at the default, supported when
allowed, the closed bound at zero), a plateau ledge (ray misses, a sphere just past the edge rolls on its corner, farther misses), undersides and two-sided triangles, a plank between the mount and the floor and a steep sheet that blocks
without letting the ground behind it through, initial overlap for a ray and a sphere, a coplanar seam crossed by 61 probes with radius 0 and 0.35, exact ties, replay and `probe_wheels` equality, every validation failure, and 800 random
probes against the exact capsule sweeps over every primitive (224 supports, 39 blocked, 460 misses, 77 initial overlaps). Eleven mutants (the slope bound and its sense, the initial-overlap detection, the sphere's placement and height, the
distance scale, the travel fraction, the direction normalization, validate-then-answer, the ray overlap normal, the center) each fail a test.

## Determinism and equality with brute force

The tree is built by median split on the widest centroid axis with the primitive id as the final tie-break, so the same inputs build the same
tree. Queries are pure functions of the geometry and the query. A capsule sweep returns *exactly* what sweeping every box with
`sweep_capsule_boxes` and every triangle with `sweep_capsule_triangles`, then taking the earlier by exact contact time (a box winning an exact
tie), would return: the hierarchy only chooses which primitives are examined, and they are examined in ascending id order. Ids number boxes
first, then triangles; an exact tie goes to the lower id. `tests/static_collision_test.cpp` checks every field of the hit against brute force on
2400 random sweeps over six worlds, 3000 ray casts against an independent oracle, region queries, ties between identical primitives and every
permutation of validation failures.

## Cost

Build is O(n log n). On a Debug build of 20000 primitives (4000 boxes, 16000 triangles) 200 local sweeps take about 0.3 ms with the hierarchy
against about 840 ms brute force; the tree is under 40 levels deep and a local sweep examines fewer than 100 primitives. These are
software-CPU numbers for scale only; no physical-hardware claim is made. Hold a `QueryScratch` across calls to avoid per-query allocation.

## Not here

Dynamic bodies, moving obstacles and streaming insertion (Core owns streaming: a chunk rebuilds or composes its own `StaticCollision`),
rotational sweeps of oriented boxes, wheel and terrain contact queries, spheres and convex hulls, and continuous collision for rigid bodies. The oriented-box overlap
and fixed-orientation sweep above are the first vehicle query; Core integrates the flat capsule-based vehicle against resident collision first and says which of the
rest it needs.
