# Navigation and pedestrians (`include/saige/navigation.hpp`, `src/navigation.cpp`)

A walkability lattice per world chunk, a map that joins the loaded chunks, A* with string pulling across them, and pedestrians that follow a path and avoid each other. Tests: `tests/navigation_test.cpp` (software build, CPU only).

* **Grid.** Points every `cell` (0.5 m) on a lattice shared by all chunks (origins are whole numbers of cells, so chunks line up exactly). A point is walkable when a ray from `search_top` above the floor finds ground within `max_slope` of level and a capsule of the agent's size, from the step height up (a 0.2 m curb is walked
  over, not an obstacle), does not overlap anything. 0.2 m from a pillar is blocked for a 0.3 m radius, 0.5 m is clear. A 20 degree ramp is walkable, 60 degrees is not; a 36 degree one is walkable ground but its steps (0.36 m between points) exceed the 0.35 m limit, so it is not climbed until the limit is raised.
* **Paths.** 8-connected, no corner cutting, octile heuristic, ties by index (deterministic), a node limit. Around a 2 m pillar the path is 16.34 m between points 16 m apart (the best path around the inflated corners is about 16.4), keeps 0.5 m from it, and is two points when nothing is in the way. A goal inside a closed wall ring or far from ground has no path.
* **Streaming.** `set_chunk` / `remove_chunk`. Two chunks on one lattice give exactly (bit for bit) the path of one grid over both; unloading the second takes the goal away and loading it again restores the same path.
* **Pedestrians.** Follow the waypoints at walking speed with bounded acceleration, are pushed apart inside 1.4 sums of radii, and sidestep to the right when the relative motion will bring them within 1.3 sums of radii inside 2.5 s (so head-on pairs form lanes). Forty pedestrians crossing a 30 m plaza around a pillar in opposite streams: all arrive in 33 s, none leaves walkable ground, none exceeds 1.4 m/s,
  the closest approach is 0.24 m (against 0.00 m without the steering; two radii are 0.56 m: they do touch in the funnel, they do not walk through each other), and a velocity change above the acceleration limit happens on 49 of 39,840 agent-ticks (a step into an unwalkable cell being stopped).

Not done: no navigation mesh (the narrowest passage is a lattice cell wide), one floor per column (the highest walkable surface below `search_top` is used: a roof is walked on; see the NAV-2 section for links and obstacles, which exist now), no hierarchical search (a 40 x 20 m region searches in well under a millisecond; a city needs a coarser level), no cost field (roads over grass) and no schedules.

## Off-mesh links, dynamic obstacles, crowds (NAV-2)

* **Off-mesh links** (`NavLink`, `NavMap::set_link`): a jump across a gap, a ladder, a door, a lift. Each end moves to the nearest walkable lattice point within a metre when a path is searched (a link with an end off walkable ground does not exist); crossing costs the link's length plus `extra_cost`, so it is taken only when it saves more than that; one-way or two-way. A path flags the point where a link starts (`NavPoint::link`), string pulling never straightens across one, and `pedestrian_step` crosses a link in a straight line at walking speed with the height following the line between the ends (a platform 1.2 m higher: half way across the link, half way up). A* keeps the plain octile estimate: a link costs at least its length, so with links the path can be longer than the optimum by at most the 8.2% between the octile and the straight-line distance.
* **Dynamic obstacles** (`NavBlocker`, `set_blocker`): a circle makes the lattice points within its radius plus the agent's unwalkable until removed; no grid is rebuilt. `path_clear` says whether a path still holds and `pedestrian_replan` goes round what has appeared (a pedestrian three seconds along a path has a blocker dropped on it, replans, never enters the blocker and arrives). An agent standing inside a small blocker starts from the nearest free point.
* **Crowds** (`crowd_step`): every agent sees only the neighbours that can matter (a uniform grid sized from the separation distance and the avoidance horizon at the current top speed), in index order, so the result is **bit-identical** to stepping each agent against all the others (checked over 120 agents and 1500 steps through the crossing) and the cost grows with density, not the square of the population.

Stress (Release, this container's CPU, `navigation_test crowd`): four streams crossing a square plaza, a crowd member every 2 m along each edge:

| Population | Plaza edge | Arrived | Time | Closest approach (bodies are 0.56 m) | Per crowd step |
| --- | --- | --- | --- | --- | --- |
| 100 | 50 m | 100 | 66.6 s | 0.34 m | 0.10 ms |
| 200 | 100 m | 200 | 126.6 s | 0.22 m | 0.24 ms |
| 400 | 200 m | 400 | 235.9 s | 0.23 m | 0.48 ms |
| 200, a member every metre | 50 m | 200 | 89.5 s | 0.18 m | 0.39 ms |

Nobody exceeds walking speed (1.40 m/s) or leaves walkable ground. The finding to keep: in the dense four-way crossing agents overlap by up to two thirds of a body (closest approach 0.18 m against 0.56 m) before the steering parts them, and the crossing takes 1.4 to 1.7 times the free-flow time. A real crowd model (reciprocal velocity obstacles or a social-force model with a body-contact term) would do better; this one is a lane-forming steering rule.

**Not done:** multiple floors in one column (a bridge over a road: two maps joined by links, no `NavWorld` yet), stairs as a distinct feature (they are walked as steps within `max_step` and ramps within `max_slope`, which is tested), destinations and daily routines for pedestrians (a schedule over places and times), traffic: a lane graph and vehicle path following for cars, intersections. None of those has code.
