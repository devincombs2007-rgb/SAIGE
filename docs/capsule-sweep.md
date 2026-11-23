# Capsule sweep primitive

`include/saige/capsule_sweep.hpp` is the collision primitive for step 1 of the first playable course (issue #62): a vertical
capsule moved through axis-aligned boxes. It is new and independent. It does not change `CharacterMotor` (#65); a motor adopts it
by converting its `StaticBox` list to `Aabb` (same layout).

## Contract

- Z is up. A capsule is a vertical segment swept by a sphere; `feet` is its lowest point and `height >= 2 * radius`.
- `sweep_capsule_box` returns the first contact while moving by `displacement`: the fraction `t` in [0, 1], a unit normal pointing
  out of the box, and the contact point on the box. It is exact, not stepped, and correct on faces, edges and corners.
- A capsule that only touches a box (within 1e-5) and is not moving into it does not hit it, so it can walk along a floor or slide
  along a wall. Pressing into a touched surface hits at `t = 0`.
- A capsule already penetrating a box reports `started_overlapping` with a push-out normal and `t = 0`. Recovery policy (how far
  to push, how to resolve several overlaps) belongs to the motor, not the primitive.
- `sweep_capsule_boxes` returns the earliest hit over a span of boxes; ties go to the lowest index, so replay is order-stable.
- `is_walkable(normal, max_slope_radians)` classifies a contact normal as ground by its angle from +Z.
- Invalid input (non-finite values, `radius <= 0`, `height < 2 * radius`, an inverted box) throws `std::invalid_argument`, like the
  motor's other validation.

## Method

A vertical segment moving by `d` first touches a box when the segment's lowest sphere center, moving along `d`, touches the box
extended downward by the segment length and rounded by the radius. That is a ray against a rounded box: a slab test against the box
grown by the radius, then, if the entry point lies in an edge or corner region, a ray against the rounded edges (cylinders with end
spheres). Normals follow from the contact configuration, so an edge gives a diagonal normal rather than a face normal. Arithmetic is
double precision on float inputs; results are bit-identical for identical inputs in one build, which is what fixed-step replay needs.

## Verification

`tests/capsule_sweep_test.cpp` checks the primitive against an independent reference. For a vertical segment and a box the distance
separates per axis, so the test finds first contact by dense sampling and an exact per-axis gap, without the Minkowski construction
the implementation uses. Six thousand randomized scenes (face, edge and corner contacts, grazes and misses) must agree on whether a
hit occurs, on `t` (within one of 3000 samples, and touching to 3e-4), and on the normal; coverage minimums keep it from passing on
an easy mix. Exact cases cover landing, head contact, rounded edges, resting contact, sliding, overlap, ties, slope limits and
invalid input. Eight deliberate mutations of the algorithm each make it fail.

## Cost

On the development container's CPU (Linux, GCC -O2, no broadphase): about 3.3 microseconds for one sweep against 128 boxes. That is
a CPU-only figure for this machine, not a statement about the target hardware; a motor does a few sweeps per tick, and a broadphase
becomes worthwhile well before the course has thousands of boxes.

## Not covered yet

Step-up, slope sliding and sticking, contact-recovery policy, moving platforms, non-box shapes, and tilted capsules. Each builds on
this primitive and is a separate slice.
