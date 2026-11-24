# Static collision: broadphase, ray casts and capsule queries

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
  against a triangle), the rule of the capsule sweeps.
* **`stop`** is the latest float center on the way to the contact at which the box is clear (separation >= 0) of *every* primitive near the path: do not recompute
  `center + displacement * t` in float. `t` is its fraction; the contact normal points from the primitive toward the box.
* **Initial overlap**: if the box already penetrates a primitive by more than the tolerance, the hit has `started_overlapping`, `t = 0`, `stop` = the start, `normal` = the
  minimum-translation direction and `penetration` its depth, whatever the displacement. If it merely touches (gap within the tolerance) it is a hit at `t = 0`
  only when the displacement moves into the surface; moving along it or away is no hit, so a box resting on a floor slides over it and drives away from a wall it touches.
* **Envelope**: as the capsule's. The box's bounding box at the start and the end must lie within +-256 m on every axis and the displacement is at most 64 m, else
  `std::invalid_argument` (also for empty geometry and for a non-finite value, a non-positive half extent or a skewed frame). Primitives may extend anywhere.
* **Snapshot scope**: shape ids are indices into the boxes and triangles the `StaticCollision` was built from, boxes first. They are valid for that immutable snapshot; a
  durable reference is the integer chunk identity plus the local shape id (Core's convention), never the index alone. Queries take a caller-owned `QueryScratch` and
  are const, so several threads may query one snapshot.

`tests/oriented_box_test.cpp` checks the sweep and the overlap depth against an oracle that shares no code with the separating-axis implementation (vertex containment
and edge-pierce tests on closed solids, with sampling and bisection for the first time of contact): 900 random sweeps against boxes and 900 against triangles agree on whether
there is a hit and on the contact time to 2e-6, the stop is clear and within 50 microns of the contact along the normal, pushing out by the reported depth separates the
box and by less does not; 800 `StaticCollision` queries (sweeps, deepest overlaps, all overlaps) equal brute force in every field; analytic edge, corner, ramp, touching
and initial-overlap cases; ties and every validation failure. Fifteen mutants (dropped axes, a flipped normal, the touching rule both ways, no initial-overlap handling,
no stop back-off, a stop clear of only the first primitive, an ignored exit time, a parallel-axis shortcut, the tie rules, the quaternion layout, the envelope) each fail a test.

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
