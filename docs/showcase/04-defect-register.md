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
            --width 1920 --height 1080 --shadow-map 4096 --frames N --out OUT          # scout8.txt: alive_scout 8 206 (format 1) or the file saige_alive writes itself; see S4-036
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
* *Master sweep (independent, whole film).* `tools/frame_sweep.py` decoded every frame and measured one-frame pulses (flicker) in 16x16 blocks. Structural flicker (blocks in which more than 25% of the pixels pulse) clusters in **three** ranges: **0:20.5-0:22.2**
  (24 blocks at the worst frame), **0:38.4-0:39.5** (28) and **0:40.8-0:42.3** (26): only the third was reported. All three are the cafe and shop interiors (pulse overlays put the red region on the wall edge); a few single frames at 1:17, 1:49 and 1:56 reach 8-11 blocks
  and are crosswalk and lane shimmer (S4-034), not surfaces.
* *Temporal regression.* Contiguous 56, 39 and 47 frame clips of those three ranges, rendered by the corrected build: **0 structural events** in each; the same clips with the film's interior-wall behaviour: **9, 6 and 15 events** (up to 23 blocks). `alive_visual` case `temporal_stability`.
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
* *Across seeds, CORRECTED (S4-036).* The cross-seed figures first reported for this entry (the film's behaviour obstructing 19, 457, 8, 43, 131 and 76 frames, the corrected director 0 on five seeds and 8 on seed 4) were measured with a **scout file that
  saige_alive misread as "no accident"** (S4-036), so they describe a director without its accident block and are withdrawn. Re-measured with the correct scout (`tests/camera_test.cpp`, 8 seeds, each with the report scouted for it and with a report that has no accident, 16 films, 5400 frames each):
  the director of `9b691ad` had the eye **inside a wall for 279 frames on seed 3 and 23 on seed 2**, the "subject hidden" rule (then judged on the point the camera looks at, which for a car is 8 m ahead of it, not on the subject: too strict, corrected in S4-038) firing on 100 frames of seed 6 and 26 of seed 3, and **19 frames obstructed on seed 34** (the coordinator's report: 20 from frame 2321; the no-accident report; the first frame was not extracted). The director now passes all 16 films with 0 frames failing any rule
  (see S4-037 and S4-038 for what changed and what it costs).
* *Framing.* The check also reports the eye inside solid geometry (0 frames on every seed) and a building between the camera and its subject, judged only for shots that follow a resident or a vehicle (the establishing and aerial shots look at a place).
* *Remaining limitation.* Not yet an engine feature: it lives in the showcase and uses showcase data. Choosing the camera position by testing candidate offsets for visibility (instead of nudging along a gradient), camera collision against the full static collision, smooth chase cameras and cinematic framing validation are P1/P3 (S4-040). Shots that follow a resident show different residents in the corrected world.

### S4-007 Lane markings on bare paving beyond the avenue's end - Low, P0, Confirmed, **Fixed**
* *Root cause.* Avenue markings started at y = -141 and the road ends at -125. *Correction.* Markings start where the road does. *Evidence.* covered by S4-003's frames.

## Open or investigated

Each entry's *Reproduction* is the recipe above with the frame given; a "not yet" means the cause is not yet known.

| ID | Film time | Severity / priority | Description | Status and evidence | Responsible subsystem | Next step and regression test planned |
| --- | --- | --- | --- | --- | --- | --- |
| S4-008 | 0:23-0:34 | Medium, P0 | Large untextured black boxes at corners | **Confirmed** visible (frames 690, 990); not identified (kiosk, bus stop or missing material: candidates, none verified) | prop models / materials | ray-cast the pixel against props and materials; test that no prop renders with a default material |
| S4-009 | whole film | High, P1 | Unstable shadows, shadow cascade popping, inconsistent shading | **Popping not reproduced; aliasing of thin shadows confirmed.** The 0:41 flicker was *not* shadows. By reading the code: cascades are stabilised (a bounding sphere per slice, centre snapped to whole texels), blended over 10% of each slice, and the renderer draws every caster it is given (no frustum culling of casters; the showcase only pre-culls draws beyond 260 m, past the 220 m shadow distance). By measurement (`tools/shadow_stability.py`, a dolly of 0.27 m per frame and a sun advancing at the film rate, shadows on minus shadows off): the shadow signal pulses on **0.38% of pixels** on the dolly and 0 with the moving sun; switching texel snapping off changes it by 0.0006 points, because at the film settings the shimmer it removes is smaller than a pixel. The pulses are thin casters (poles) aliasing. | renderer shadows | temporal anti-aliasing (the CPU reference exists, `render_reference/taa`); a diagnostic overlay of cascade boundaries; the test guards the aliasing floor (0.6%), it does not detect snapping |
| S4-010 | 0:37-0:42 | High, P1 | Noisy speckled vehicle reflections; opaque bright windshields; inconsistent window transparency; unconvincing glass | **Confirmed visible** (car body speckle, flat white windshield in frames 1110-1260); cause not known. The scene has no image-based lighting and no specular environment, which would explain flat glass but not the speckle (not verified) | materials, IBL, alpha blending | one-car capture isolating paint and glass parameters and the environment term; wire `claude/ibl-gpu` if that is the cause |
| S4-011 | whole film | High, P0 | Road and navigation graph inconsistencies; lane connectivity | **Partly fixed.** Validator rules added for lane paths off the roadway, walker nodes on the road or blocked by a collision box, walker edges through buildings, and building collision. They found two defects in the generator, both fixed: the 18 **entry lanes began up to 15 m beyond the end of the drawn roadway** (`boundary_run` 80 m against a 65 m road), so cars entered where the data had no road; and **park trees placed at random could stand on the pedestrian path** (seed 10), now sharing one definition of the path between navigation and tree placement (`park_path_points`). 0 findings on 16 seeds. **Not validated:** lane successor connectivity, crosswalk-to-node alignment, one-way and turn consistency | district generator, traffic | lane-graph connectivity and crosswalk alignment checks |
| S4-012 | interiors | Medium, P0 | Staircase clearance; missing stair railings; inaccessible upper floors | **Partly addressed** (landings kept clear). Not verified: head clearance along the stair, a railing (none is modelled: confirmed by reading `build_interior`), reachability of the upper floor through the nav graph | interiors, multi-floor nav | headroom and reachability checks; model a railing |
| S4-013 | whole film | High, P1 | Foot sliding, stride sync, turning, curb and stair traversal, foot penetration, clipping, abrupt transitions, posture, avoidance | **Suspected.** Nothing measured on the film. The engine has foot locking and IK (C1d) that Showcase 04 did not use | character animation | per-clip foot-slide measurement against root speed in a targeted scene; wire `FootPlacement`; test foot contact |
| S4-014 | whole film | High, P1 | Wheel rotation, steering, suspension, collision synchronisation | **Suspected.** Not measured | vehicle | drive a short scripted course and compare wheel angular speed with chassis speed, steering with lane curvature, visual body with physics body, frame by frame |
| S4-015 | about 2:00-3:00 | High, P1 | No local illumination (street lights, headlights, brake lights, emergency lights, interiors); flat interior lighting; emissive mistaken for light | **Confirmed**: the renderer used has no point or spot lights; emissive + bloom light nothing. **Started:** the CPU reference for point and spot lights (photometric units, windowed inverse-square falloff, spot cone; 16 tests, 18 of 19 mutants caught, the survivor equivalent) and the design are on `claude/fidelity-dev` (`docs/rendering/local-lights.md`). No shader, light buffer or culling pass yet | renderer lighting | the shading function in `pbr.hlsli` checked against the reference, then clustered forward culling on the RHI compute pipeline, then headlights/streetlights/interiors; light-contribution diagnostic mode |
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
| S4-031 | whole film | Medium, P0 | Crosswalks terminating at raised curbs; road and sidewalk discontinuities | **Confirmed by data**: every one of the **33 crosswalks** meets the sidewalk at a 0.15 m kerb at both ends, and the data has no ramp (`CROSSWALK_CURB_STEP`, a warning). Not fixed: ramps need render geometry, collision and `ground_height` to agree (the sidewalk collision is a 0.15 m box) | district/ground/collision | dropped kerbs generated from the crosswalks, drawn, collided and walked from one source; the warning becomes an error |
| S4-032 | interiors | Medium, P2 | Insufficient storefront and interior detail; flat interior illumination | **Confirmed**; depends on lights (S4-015) | interiors | after lights |
| S4-033 | transient | - | Additional defects the review did not list (transient motion, audio) | **Swept, partly examined.** The whole master was decoded (5398 frames): 83 flicker and 130 popping events. Flicker classified (S4-001: three structural clusters; the rest thin-feature aliasing, S4-034). The popping events cluster at 0:20-0:22 (the same interior flicker), 1:43-1:52 and 2:23-2:36 (the ambulance and the dusk aerial: moving vehicles and emissive windows against a steady frame) and were **not individually reviewed**; no popping surface has been confirmed beyond S4-001 | QA | review the 1:43-1:52 and 2:23-2:36 pop events frame by frame; add the sweep to CI on a short corrected clip |
| S4-034 | whole film (lane markings, crosswalk stripes, fences, window frames) | Medium, P1 | Temporal aliasing: thin features shimmer | **Confirmed** (pulse overlays: scattered red pixels on markings at 1:21 and 1:56, no structure). No temporal anti-aliasing in the film | renderer AA | TAA (CPU reference done: jitter, reprojection, neighbourhood clipping) |
| S4-035 | whole film | Medium, P0 | The director's shots are tied to the world they were scouted on | **Confirmed.** `scout8.txt` was generated for the film's world; after any world-data change the accident happens at a different time and follow shots pick different residents, so frames of the corrected world are not the film's frames. The tests therefore compare the corrected binary with itself, and the cross-seed tool scouts every seed fresh | tooling | none needed for correctness; documented so that nobody compares frames across a world change |

| S4-036 | tooling (all cross-seed and visual-regression runs of the previous round) | High, P0 | The scout file the tools wrote was not the file the app read | **Confirmed, reproduced.** `alive_scout 8 230 > scout8.txt` (the documented recipe) wrote a one-line human summary; `saige_alive` read the word `accident` as a record tag, read `at` as a number, failed, and ran the director with accident time 0 and no response: a different shot list from the second shot on (shot at 12.0 s "cafe terrace" vs the real 12.0..22.2 timeline). `tools/camera_seeds.py` and `tools/visual_regression.py` followed the recipe, so their cross-seed results and, on hosted CI, the frames of the visual cases came from a director without its accident (hosted run 654: the `scenery_does_not_cover_the_road` mutant was NOT caught at "frames 1110 and 1170", which were other shots). The delivered film is not affected: its own `scout8.txt` on disk is in the app's layout (accident 106.2 s) | tooling | one writer and one strict parser (`src/scout.cpp`, format 1: header, seed, accident record, events with energy, end line with the count); the app refuses anything else and never replaces a file it cannot read; `alive_scout` writes the same format with the app's own world configuration; `--scout-legacy` reads the headerless layout of the film's file; `tests/scout_test.cpp` (round trip from the simulation, the director's choice of the accident, the film's legacy file, the human summary refused, 38 malformed inputs, every prefix of a valid file); mutation 17 of 17; visual case `scout_producers_agree` (the app's file equals alive_scout's, byte for byte, and a human summary is refused). Provenance caveat: the film's file regenerated by today's binary has on-scene 152.000 s where the file on disk has 152.033 s (accident, vehicle and shot list identical); that the render read exactly those bytes is not proven by anything kept with the film |
| S4-037 | director (accident in the first 49 s of a film) | High, P0 | The accident block started inside the intro | **Confirmed** (seeds 2 and 6, accidents at film 8.8 s and 21.3 s). The intro always took at least 40 s, so the wreck, the street, the ambulance shots started in the middle of it and the sorted timeline interleaved them (wreck, crossing, street reacts, ...); the shot that follows the doomed car played after the crash. | director | the intro ends where the run-up does and keeps only the opening shots that fit; an accident before 2.5 s of film is not given a block. Unchanged for accidents later than 49 s (seed 8's timeline is the same) | `alive_camera` 6:real |
| S4-038 | director (camera clearance) | High, P0 | The 3 m (later 5 m) displacement cap did not bound the camera | **Confirmed by the director's review and by measurement.** The push-outs (geometry, canopy, awning) each had their own origin and `camera()` ran the chain twice, so nothing bounded the sum; and a push out of one wall could land in the neighbouring building (seed 3: 279 frames with the eye in a wall, in the gap between two abutting buildings) | director | one search per frame over offsets from what the shot proposed (horizontal 5 m, up to 2.5 m up), judged by the same three rules as the check (eye in a wall or prop with a 0.35 m margin, foliage/awning in view, a building between the eye and the subject itself, not the look-ahead point), keeping the standing offset while it passes and relaxing it smoothly. Measured independently of the director's bookkeeping (`tests/camera_test.cpp`): 16 films, 0 failing frames, 0 unresolved. **Costs, reported not hidden:** frames beyond the 5 m budget (only to leave a wall): seed 2 real 144 frames (6.0 m), seed 3 real 76 (5.6 m), seed 4 real 6 (5.4 m), none elsewhere; the eye jumps when the standing offset stops passing: 1 to 2 jumps per film over 1.5 m on seeds 34, 4, 5, the largest 7.3 m (seed 4, 'at the scene'; 5.4 m seed 34, 'follow a resident'), a cut the shot did not ask for. Shots that follow a resident show different residents than the film | `alive_camera` (default 4 films, `--all` 16), mutants: the film's behaviour (`SAIGE_ALIVE_NO_CANOPY_CLEAR`) and a 1 m budget must fail |
| S4-039 | tooling | Medium, P0 | Visual regression could skip, run nothing, or run with a mutation switch inherited from the shell | **Confirmed.** unknown `--only` ran zero cases and passed; an inherited `SAIGE_ALIVE_*` switch changed a "baseline" render; hosted CI had no ffmpeg (run 654 stopped at `temporal_stability`) | tooling | unknown `--only`, a required run without `--mutants`, zero executed tests and an intended mutant not caught all fail; every inherited `SAIGE_ALIVE_*` is removed and reported; frame cases use fixed stills (the film cameras of the frames, independent of the director and the simulation); `camera_seeds.py` fails on an empty or duplicated seed list and has budget 0 |

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

## Test infrastructure: assets and CI

The visual cases need the third-party textures and models the renderer reads; before, a machine without them skipped the tests (exit 77) and CI reported a pass. Now:
* `showcase/04-world-alive/assets.lock.json` pins the 149 files the renderer opens (found by tracing a run with `strace -f -e trace=openat`): SHA-256, size and licence (Poly Haven CC0, the Khronos Car Concept CC BY 4.0), 87 MB at 1k.
* `tools/restore_assets.py --restore DIR` downloads them from their owners (Poly Haven's documented API, raw.githubusercontent.com) and verifies every file; a mismatch is an error and the file is removed (an upstream change is reviewed, not accepted). Tested from an empty directory: 149 of 149 files, and a frame rendered from the restored cache is **pixel-identical** to the one from the original cache.
* `SAIGE_REQUIRE_VISUAL=1` turns "renderer or assets missing" into a failure in `visual_regression.py` and `camera_seeds.py`. The hosted Graphics workflow (lavapipe job) restores the assets (cached by the lock's hash), runs the visual regression with its mutants and sets that variable, so the step cannot skip silently. The humanoid library is restored by the existing `tools/animation/fetch_humanoid_assets.py`.
* Caveat: the restore needs network access to Poly Haven and GitHub; if either is down the job fails (visibly) rather than skipping. Hosted run 654 (`9b691ad`) was the first to run it: the restore and the first three cases rendered; the run then failed at `temporal_stability` (no ffmpeg on the runner: environmental, now installed) after the scenery mutant was not caught (the scout mismatch, S4-036).
* **Fixture path** (`visual_regression.py --fixtures`, required in the Graphics workflow, nothing downloaded): `tools/make_fixtures.py` generates the 126 textures from the lock's file names (64 x 64 PNG data under the `.jpg` names; a colour fixed by the material's folder, an 8 px checker and a ramp; flat normal; flat ARM), the app's `--fixtures` draws a procedural sedan instead of the Khronos car and no residents. Deterministic (same digest on every run). **What it covers:** geometry, depth precision and coplanar surfaces, shadows, scenery, the director and the camera rules, all with real materials' colours replaced by fixed ones. **What it does not cover:** the third-party textures' detail (normal and ARM maps are flat), the Khronos car model and its wheel rig, the props (hydrant, bench, bin, lamp, tables, chairs, plants, sofa, crates, planters), the residents and their animation. The full-asset run (restore, then `visual_regression.py` without `--fixtures`) keeps those and is a separate step. Fixture results are **not** evidence of full-asset fidelity.
* Every intended mutant must be caught: interior walls, hidden faces, scenery, temporal flicker, camera clearance, and a human summary read as a scout file (6 of 6 on the local fixture run).

## Tests that guard the corrections

* `alive_validate` (`validate_test.cpp`): 16 generated districts have no blocking finding; every defect class is injected into a clean district and must be reported; exact geometry cases.
* `alive_scout` (`tests/scout_test.cpp`): the scout file format, the director's choice of the accident, malformed input; `alive_camera` (`tests/camera_test.cpp`): the director's camera on whole films without a renderer, with two mutants that must fail.
* `alive_visual` (`tools/visual_regression.py`): renders fixed cameras of the film twice with one controlled difference and compares pictures, plus a dolly of 41 stills swept for structural flicker (`temporal_stability`) and `scout_producers_agree`; `--mutants` switches each film behaviour back on and requires the case to fail.
  Full-asset run: needs the renderer and the Poly Haven cache (restored in the Graphics workflow). Fixture run (`--fixtures`): required in the Graphics workflow. **Software Vulkan only: nothing here is physical-GPU evidence.**
* `tools/camera_seeds.py` (the same rules through the app, one seed at a time, budget 0; the C++ test is the primary check), `tools/shadow_stability.py` (the shadows' own pulse on a dolly and under a moving sun), `tools/frame_sweep.py` (flicker and popping in any video; `--assert-no-structural-flicker`).
* `alive_district`, `alive_world`, `alive_traffic`: passing after the corrections.

A passing CTest run is not sufficient on its own: the visual cases exist because the numeric tests passed on the film's data while the picture was wrong.
