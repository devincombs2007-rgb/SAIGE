# Showcase 04 defect register

Permanent record of the defects in the delivered Showcase 04 film ("SAIGE - A World Alive": master `SAIGE_A_World_Alive.mp4`, SHA-256 `00dcf1fdd2fc33c7535d2d743fc81a7ea46f10aa1d66201f020a958c6ee3e312`, engine
source `50ca1f3`), found by the development director's review and by our own inspection. **The film is finished and is not remade, re-rendered, remastered or modified**: every correction is to the engine,
the production tools or the showcase's world source, and is shown with short single-frame captures and guarded by tests. Branch of the corrections: `claude/s04-quality`. Showcase 05 is a separate production,
authorised later by the owner; it is not started.

**Statuses.** *Confirmed*: reproduced from source on the film's own frame. *Suspected*: reported or inferred, not yet reproduced (a suspected cause is never recorded as a root cause). Resolution: *Fixed* (cause
found, change made, regression test fails without the change: the mutant is caught), *Root-caused*, *Investigated* (evidence, cause unknown), *Open*.
**Severity.** Critical: visibly wrong geometry or rendering in the film. High: wrong behaviour or a large realism gap. Medium: noticeable. Low: minor or latent.
**Priorities** follow the owner's order: P0 critical correctness, P1 foundational realism, P2 advanced fidelity, P3 scalability and refinement (see `docs/engineering/backlog.md`).

**How to reproduce any entry.** The same frame of the same seed, from source (about 4 s per frame at 1080p on software Vulkan; the film's frame number is `seconds x 30`):

```
saige_alive --film --seed 8 --film-seconds 180 --preroll 25 --fps 30 --time-scale 47 --scout-file scout8.txt \
            --width 1920 --height 1080 --shadow-map 4096 --frames N --out OUT          # scout8.txt: alive_scout 8 230
SAIGE_ALIVE_LEGACY_INTERIOR_WALLS=1 | SAIGE_ALIVE_LEGACY_HIDDEN=1 | SAIGE_ALIVE_LEGACY_SCENERY=1 | SAIGE_ALIVE_NO_CANOPY_CLEAR=1     # switch the film's behaviour back on
SAIGE_ALIVE_SKIP=interior,roof,frames,...  SAIGE_ALIVE_NOCOPIES=1  SAIGE_ALIVE_ALLFACES=1  --disable shadows,ao,bloom,fxaa  --check-camera     # controlled experiments
```

The corrected world changes the simulation (props and furniture are different, so crowds and cars diverge after the first minutes); frames compared "before and after" are therefore only comparable where the
shot's subject is the same, and the guarding tests compare the corrected binary with itself under a controlled difference instead.

## Corrected and guarded

### S4-001 Facade flicker (0:41-0:42; any view of the five modelled interiors) - Critical, P0, Confirmed, **Fixed**
* *Description.* Patches of a second texture over a brick wall that change every frame, strongest at a grazing sun.
* *Reproduction.* Frame 1245 (0:41.5), crop of the left wall. Experiments on that frame: shadows, AO, bloom and FXAA off in turn: unchanged; dropping the interior layer (`SAIGE_ALIVE_SKIP=interior`): gone.
  The first suspicion (shadow acne at a 9 degree sun) was wrong.
* *Root cause.* `build_interior` emitted each interior wall as a full box whose outer face lies exactly on the footprint edge: the **same plane** as the facade's outer wall quad, with a different material.
  Two coplanar surfaces cannot be ordered by a depth buffer (the engine's is D32 float, reverse-Z: it is not a precision problem). Affects the cafe, shop, bakery, bar and pharmacy, in the district and in all six scenery copies.
* *Subsystem.* Showcase scene assembly (`render/city.cpp`), world authoring.
* *Correction.* `interior_wall_face_mask`: a wall box never draws a face lying on the footprint boundary (it belongs to the facade).
* *Regression test.* `alive_visual` case `interior_never_changes_exterior`: 0.000% of the region differs with and without interiors; the film's behaviour (mutant) differs by 19.4% (caught). `alive_validate` `test_interior_wall_face_mask`.
* *Visual evidence.* `docs/showcase/evidence/s04-001-005-before-after.jpg`, row 1.
* *Remaining limitation.* The validator checks overlap at footprint level (`WALL_COPLANAR`), not mesh level: same-material boxes that overlap at building corners (the cornice boxes extend 0.22 m past each face, so they overlap at corners; reasoned from the code, not verified) would not be reported.

### S4-002 Missing building walls (0:23-0:34; 1:50-1:54) - Critical, P0, Confirmed, **Fixed**
* *Description.* An upper floor with nothing below it, a dark gap at the foot of a wall; at 1:54 a bright vertical gap between two buildings showing lit geometry behind.
* *Reproduction.* Frames 690, 990 (0:23, 0:33) and 3420 (1:54): the film shows the gap; `SAIGE_ALIVE_LEGACY_HIDDEN=1` reproduces it. Frame 3420 differs from the corrected render in a 14 px wide vertical sliver exactly where the film shows lit colour through the gap.
  Other frames sampled between 1:50 and 1:54 (3300-3408, every 12) differ by about 1% (video compression noise at edges); the 1:54 gap is the only structural difference found there.
* *Root cause.* The renderer dropped a **whole** face when ONE sample point, 1 m outside its midpoint, was inside a neighbouring building. A building next to a shallower one lost the wall that stands exposed beyond the neighbour.
  On the film seed the rule dropped **52 faces carrying 276 m of exposed wall**.
* *Subsystem.* Showcase scene assembly + district data.
* *Correction.* `exposed_face_spans` (exact intervals of a face not pressed against a neighbour); a wall piece is emitted per exposed span.
* *Regression test.* `alive_visual` `walls_complete` (frames 690, 990: 0.05% differing from the render with every face whole; the film's rule 1.2-2.0%, caught); `alive_validate` `test_exposed_face_spans`, `test_film_rule_dropped_exposed_walls_on_the_film_seed`.
* *Visual evidence.* `s04-001-005-before-after.jpg` row 2; `s04-002b-gap-at-1m54.jpg` (film left, corrected right).
* *Remaining limitation.* Faces partly covered by a neighbour get no awning, balcony, sign or cornice (they belong to whole faces). Wrong triangle winding (a wall invisible from outside) is not detected by any test yet.

### S4-003 Car apparently driving over a raised pedestrian surface (0:37-0:40) - Critical, P0, Confirmed, **Fixed** (the cause was not physics)
* *Description.* A raised sidewalk-coloured slab across the avenue; the blue car drives across it.
* *Reproduction.* Frames 1110, 1170 (0:37, 0:39). With `SAIGE_ALIVE_NOCOPIES=1` the road is continuous and clean.
* *Root cause.* **Visual only.** The simulation's collision set contains no scenery. `CityScene::collect` drew six shifted copies of the whole district as scenery, including the ground, quay, promenade and street sidewalks (raised 15 cm);
  the copy shifted 180 m south laid its waterfront and cross-street sidewalks over the real avenue's last 15 m. Road generation, collision geometry, lane connectivity, steering and navigation were not the origin
  (the car's path is on the avenue's own lanes; only the picture was wrong).
* *Subsystem.* Showcase scene assembly (scenery tiling).
* *Correction.* The scenery has its own ground clipped where the real avenues' road ends (local y <= 55), without waterfront, piers, quay or sea bed; scenery props beyond that line are not drawn; lane lines stop where the road stops.
* *Regression test.* `alive_visual` `scenery_does_not_cover_the_road` (frames 1110, 1170: the lower 42% of the picture is identical with and without scenery; the film's scenery changes 74.9%, caught).
* *Visual evidence.* `s04-001-005-before-after.jpg` row 3.
* *Remaining limitation.* The scenery is still copies of the same blocks (repetitive); the seam where the real avenue meets the scenery is a straight cut. Crosswalks ending at raised curbs (reported) are a separate entry (S4-031).

### S4-004 Furniture in parking spaces and on the roadway (whole film) - High, P0, Confirmed, **Fixed**
* *Reproduction.* `alive_validate -v 8` on the film's code: `PROP_ON_ROAD` 122, `PROP_IN_PARKING` 63, `PROP_OVERLAP` 24.
* *Root cause.* Cafe tables were placed 4.2 m from the facade; the sidewalk is 3.5 m deep, so tables and chairs stood in the parking bay and 0.7 m onto the asphalt, overlapping parked cars (**34 tables, 68 chairs on the roadway, 61 pieces in bays, 23 on parked cars**).
  Benches placed across the sidewalk also reached the asphalt (20).
* *Subsystem.* District generator (`build_props`).
* *Correction.* Tables 0.8 m from the facade; benches parallel to the kerb; park trees, bus stop and bike racks placed only where free.
* *Regression test.* `alive_validate`: 212 findings on the film seed before, 0 now, 0 on 16 seeds; each rule is injected into a clean district and must fire.
* *Remaining limitation.* Props are placed by rejection, not by an authored layout; density of street furniture is lower than a real street's.

### S4-005 Camera inside foliage (1:30-1:33) - High, P0, Confirmed, **Fixed**
* *Reproduction.* `--check-camera` with `SAIGE_ALIVE_NO_CANOPY_CLEAR=1`: 90 of 5400 frames from 90.2 s (the film's world, trunk-only collision).
* *Root cause.* The director kept the eye out of the collision set, which holds a tree's trunk (0.44 m); the canopy (five spheres, about 1.4 to 7 m above ground) was known only to the mesh builder.
* *Subsystem.* Showcase director; the canopy is now shared data (`vegetation_shape.hpp`).
* *Correction.* The director pushes the eye out of canopy spheres; see S4-024 for the general obstruction rule.
* *Regression test.* `alive_visual` `camera_clear_of_foliage` (0 of 5400 frames; mutant caught). *Evidence.* `s04-camera-before-after.jpg`.

### S4-006 Interiors that cannot be used - High, P0, Confirmed, **Fixed**
* *Root cause.* Hand-written layouts ignore the building's size and door position: a chair through a side wall (bar), a counter in the doorway (shop), a display case in the doorway (bakery), a chair on the top landing of the cafe's stairs.
* *Correction.* Layouts keep the door's path clear and every layout is pruned against the clearance rules (inside the room, off the walls, off the doorway strip, off the stairs and landings).
* *Regression test.* `alive_validate` rules `ROOM_FURNITURE_*`, `DOOR_BLOCKED`, `STAIR_BLOCKED`, `STAIR_TOP_BLOCKED`, each shown to fire on an injected defect; 0 findings on 16 seeds.
* *Remaining limitation.* Pruning removes furniture, so some interiors are emptier (the directive's "insufficient interior detail" is still open: S4-032).

### S4-023 Tree leaves through buildings and awnings (whole film; reported as "trees intersecting or approaching buildings and awnings") - High, P0, Confirmed, **Fixed**
* *Reproduction.* `alive_validate 8` after adding the canopy rule: `TREE_CANOPY_ON_BUILDING` 48, `TREE_CANOPY_ON_AWNING` 7 (street trees and park trees together). Frames 1635, 1665 (0:54, 0:55) show a canopy touching the awning.
* *Root cause.* Trunks stood 2.75 m from the facade; the canopy reaches up to about 3.5 m (five spheres around the trunk).
* *Subsystem.* District generator (`plant_tree`).
* *Correction.* Trees stand kerbside; several orientations and sizes are tried, then a smaller tree, then none (17 trees were dropped).
* *Regression test.* `alive_validate` (`TREE_CANOPY_ON_*`, 0 on 16 seeds; injected tree fires; `test_trees_and_awnings`).
* *Remaining limitation.* The trees are still five-sphere blobs (S4-019).

### S4-024 Camera obstructed by awnings and foliage (0:37, 0:53-0:56, 1:30) - High, P0, Confirmed, **Fixed**
* *Reproduction.* `--check-camera` with `SAIGE_ALIVE_NO_CANOPY_CLEAR=1` on the corrected world: 124 of 5400 frames (from 36.97 s and 53.63 s); the film's frames 1635 and 1665 show a red awning slab and a canopy across the top of the frame.
* *Root cause.* Awnings are not in the collision set (nothing walks into them) and the director tested only the eye point; a camera at 2.05 m under a 2.22 m awning has the awning's edge in the frame. A first correction (eye-inside test with a 0.2 m margin, push-out
  with no cap) dragged the camera up to 19 m from its subject; it was replaced before it shipped.
* *Subsystem.* Showcase director -> `view_obstructed` (district data: awning boxes and canopy spheres; reusable by any camera).
* *Correction.* Nine rays across the frustum; obstructed when the eye is inside leaves or an awning, or at least two rays meet an awning within 3 m or leaves within 4 m. The eye steps toward the street side in 0.3 m steps, never more than 3 m in all.
* *Regression test.* `alive_visual` `camera_clear_of_foliage` (0 of 5400; mutant caught); `alive_validate` `test_view_obstruction`.
* *Remaining limitation.* Not yet an engine feature: it lives in the showcase and uses showcase data. Camera collision against the full static collision, smooth chase cameras and cinematic framing validation are P1/P3 (S4-040). Shots that follow a resident show different residents in the corrected world.

### S4-007 Lane markings on bare paving beyond the avenue's end - Low, P0, Confirmed, **Fixed**
* *Root cause.* Avenue markings started at y = -141 and the road ends at -125. *Correction.* Markings start where the road does. *Evidence.* covered by S4-003's frames.

## Open or investigated

Each entry's *Reproduction* is the recipe above with the frame given; a "not yet" means the cause is not yet known.

| ID | Film time | Severity / priority | Description | Status and evidence | Responsible subsystem | Next step and regression test planned |
| --- | --- | --- | --- | --- | --- | --- |
| S4-008 | 0:23-0:34 | Medium, P0 | Large untextured black boxes at corners | **Confirmed** visible (frames 690, 990); not identified (kiosk, bus stop or missing material: candidates, none verified) | prop models / materials | ray-cast the pixel against props and materials; test that no prop renders with a default material |
| S4-009 | whole film | High, P1 | Unstable shadows, shadow cascade popping, inconsistent shading | **Suspected.** The 0:41 flicker was *not* shadows (shadows off: unchanged). No cascade popping measured yet; the film used 3 cascades, 4096, 220 m, 1x1 PCF | renderer shadows | diagnostic mode: cascade boundary overlay; consecutive-frame stability test on a moving sun with a static camera |
| S4-010 | 0:37-0:42 | High, P1 | Noisy speckled vehicle reflections; opaque bright windshields; inconsistent window transparency; unconvincing glass | **Confirmed visible** (car body speckle, flat white windshield in frames 1110-1260); cause not known. The scene has no image-based lighting and no specular environment, which would explain flat glass but not the speckle (not verified) | materials, IBL, alpha blending | one-car capture isolating paint and glass parameters and the environment term; wire `claude/ibl-gpu` if that is the cause |
| S4-011 | whole film | High, P0 | Road and navigation graph inconsistencies; lane connectivity | **Partly addressed.** The validator covers buildings, roads, sidewalks, parking, props, interiors; `alive_district_test` checks pedestrian nodes against collision. **Not validated:** lane-graph connectivity, crosswalk-to-node alignment | district generator, traffic | add lane graph and crosswalk checks to the validator |
| S4-012 | interiors | Medium, P0 | Staircase clearance; missing stair railings; inaccessible upper floors | **Partly addressed** (landings kept clear). Not verified: head clearance along the stair, a railing (none is modelled: confirmed by reading `build_interior`), reachability of the upper floor through the nav graph | interiors, multi-floor nav | headroom and reachability checks; model a railing |
| S4-013 | whole film | High, P1 | Foot sliding, stride sync, turning, curb and stair traversal, foot penetration, clipping, abrupt transitions, posture, avoidance | **Suspected.** Nothing measured on the film. The engine has foot locking and IK (C1d) that Showcase 04 did not use | character animation | per-clip foot-slide measurement against root speed in a targeted scene; wire `FootPlacement`; test foot contact |
| S4-014 | whole film | High, P1 | Wheel rotation, steering, suspension, collision synchronisation | **Suspected.** Not measured | vehicle | drive a short scripted course and compare wheel angular speed with chassis speed, steering with lane curvature, visual body with physics body, frame by frame |
| S4-015 | about 2:00-3:00 | High, P1 | No local illumination (street lights, headlights, brake lights, emergency lights, interiors); flat interior lighting; emissive mistaken for light | **Confirmed**: the renderer used has no point or spot lights; emissive + bloom light nothing | renderer lighting | point and spot lights, clustered assignment on compute (RHI program), then headlights/streetlights/interiors; light-contribution diagnostic mode |
| S4-016 | whole film | High, P3 | Population density (60 residents, about 14 vehicles) | **Confirmed.** Cost of raising it not measured | simulation | measure 2x/4x on `alive_world_test`; raise with the validator and world test as the gate |
| S4-017 | about 1:45-2:05 | Medium, P1 | Emergency interaction not visible | **Confirmed** (the source: 22 m "on scene", 25 s clearing, wreck removed not towed, no treatment, crumple visual) | world simulation | incident as activities (S4-050) |
| S4-018 | 1:30-1:35 | Medium, P2 | Smoke is stacked translucent discs with visible volume boundaries | **Confirmed** (frames 2760+) | effects | particle/volume smoke |
| S4-019 | whole film | Medium, P2 | Low-poly trees, simple vehicles, mannequin characters, repetitive buildings, flat materials, empty interiors, sparse detail, inconsistent style | **Confirmed**, subjective in degree | assets | professional asset pipeline (S4-060) |
| S4-020 | latent | Low, P0 | The facade plinth box is created with face mask 0 and never draws | **Confirmed** by reading `build_building` | scene assembly | draw it or remove it |
| S4-021 | whole film | Medium, P1 | Audio | **Measured, not listened to.** No clipping (peak -1.0 dBFS), no silence in the film, no DC offset (5e-5); short-term loudness -21 to -13.7 LUFS (7 LU swing). A person's listening notes on sync, mixing and repetition are required | audio | listening review; sync test from the audio log |
| S4-025 | day to night | Medium, P1 | Inconsistent exposure across the transitions | **Suspected.** Exposure is a hand-written function of sun elevation (`exposure_for`); no measurement | renderer tone mapping | log mean luminance per frame across the film's range and test continuity |
| S4-026 | 0:00 | Medium, P2 | Abrupt terrain boundaries; poor shoreline | **Confirmed** visible (frame 12 s: a straight quay edge, flat sea bed) | environment | shoreline and terrain transition |
| S4-027 | whole film | Medium, P0 | Repetitive building designs, inconsistent proportions, façade materials | **Confirmed** (a palette of 6 and a kit of window shapes; 4 floor heights) | district generator | authored building kit with variation; validator rule on proportions |
| S4-028 | whole film | Medium, P2 | Water: decorative boats, no wakes/spray/foam, no night illumination of water | **Confirmed** | water | WATER-3 |
| S4-029 | whole film | Medium, P2 | Vegetation without wind; no weather | **Confirmed**; engine CPU modules exist on `claude/fidelity-dev`, not integrated | environment | integration |
| S4-030 | whole film | Medium, P1 | Vehicle crumple is visual only; no progressive deformation or breakable parts | **Confirmed** | vehicle damage | VEH-1 follow-ups |
| S4-031 | whole film | Medium, P0 | Crosswalks terminating at raised curbs; road and sidewalk discontinuities | **Suspected** (reported); not yet reproduced | district/ground | curb-ramp rule in the validator; capture at a crosswalk |
| S4-032 | interiors | Medium, P2 | Insufficient storefront and interior detail; flat interior illumination | **Confirmed**; depends on lights (S4-015) | interiors | after lights |
| S4-033 | transient | - | Additional defects the review did not list (transient motion, audio) | **Not searched.** The master was inspected at the reported times and at ten points; a frame-by-frame sweep has not been done | QA | automated frame differencing (flicker, popping, intersections, stuck actors), then review the flagged frames |

## The validator and its contract (for the world editor)

`showcase/04-world-alive/src/district_validate.{hpp,cpp}`, run by `alive_validate [seed...] [-v]` and by `alive_validate_test`. Contract version 1. It reads `District` (data) only, never meshes, so any authoring tool that can produce a `District`
gets the same checks; a finding has a stable `code`, a `severity`, a position and the ids involved. Coordination with Gemini's world editor is on issue #43: the editor should call `validate_district` on every save and refuse to publish `error` or
`critical`; new rules are added with a test that injects the defect.

| Code | Severity | Meaning | `ids` |
| --- | --- | --- | --- |
| `BLD_OVERLAP` | critical | building footprints overlap | building, building |
| `BLD_ON_ROAD`, `BLD_ON_SIDEWALK` | critical, error | a building stands on the roadway or the sidewalk | building |
| `WALL_COPLANAR` | critical | two buildings' faces lie in one plane with overlapping extent | building, building |
| `PROP_ON_ROAD`, `PROP_IN_PARKING` | error | an outdoor prop on the roadway or in a parking bay | prop, (spot) |
| `PROP_IN_BUILDING` | error | an outdoor prop inside a footprint | prop, building |
| `PROP_OVERLAP` | error / warning | two props overlap (> 0.02 m2 error) | prop, prop |
| `TREE_CANOPY_ON_BUILDING`, `TREE_CANOPY_ON_AWNING` | error | a tree's leaves inside a building or an awning | prop, building / awning |
| `PARKING_ON_BUILDING`, `PARKING_ON_SIDEWALK`, `PARKING_OFF_ROAD` | error, warning, warning | a bay overlaps a building or the sidewalk, or is not on the road | spot, (building) |
| `ROOM_FURNITURE_OUTSIDE`, `ROOM_FURNITURE_IN_WALL`, `ROOM_FURNITURE_OVERLAP` | error | furniture through a wall, in a wall, overlapping | building, piece, (piece) |
| `DOOR_BLOCKED`, `STAIR_BLOCKED`, `STAIR_TOP_BLOCKED` | error | furniture in the doorway, on the stairs or foot landing, on the top landing | building, piece |

Also exported: `exposed_face_spans`, `face_frame`, `interior_wall_face_mask`, `awning_boxes`, `view_obstructed`, `tree_clear_of_buildings`, and `vegetation_shape.hpp` (the canopy as spheres).
Observation worth keeping: on the film seed no building footprints overlap and no two buildings share a plane (`BLD_OVERLAP` and `WALL_COPLANAR` are 0 on 16 seeds): the "overlapping façade surfaces" were the interior walls and the scenery, not the buildings.

## Tests that guard the corrections

* `alive_validate` (`validate_test.cpp`): 16 generated districts have no blocking finding; every defect class is injected into a clean district and must be reported; exact geometry cases.
* `alive_visual` (`tools/visual_regression.py`): renders film frames twice with one controlled difference and compares pictures; `--mutants` switches each film behaviour back on and requires the case to fail (4 of 4 caught).
  Needs the renderer and the Poly Haven cache; skipped (exit 77) otherwise, so hosted CI does not run it. **Software Vulkan only: nothing here is physical-GPU evidence.**
* `alive_district`, `alive_world`, `alive_traffic`: passing after the corrections.

A passing CTest run is not sufficient on its own: the visual cases exist because the numeric tests passed on the film's data while the picture was wrong.
