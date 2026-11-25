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

Capsule queries share the envelope of `capsule_sweep.hpp` (chunk-local, within 256 m per axis, at most 64 m long, otherwise
`std::invalid_argument`, even when the geometry is empty). The geometry itself may extend anywhere.

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
shape queries beyond the vertical capsule (spheres, oriented boxes for vehicles) and continuous collision for rigid bodies. Vehicle collision
needs an oriented-box or convex query against this structure; that is the next primitive to agree with Core.
