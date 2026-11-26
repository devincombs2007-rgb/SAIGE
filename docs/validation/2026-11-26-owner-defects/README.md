# Owner-reported defects: first reproductions (2026-11-26)

Branch `claude/s04-quality`. The film is not touched; these are corrections to the world generator and the renderer's placement code, with validators.

| Owner report | Status | Evidence |
| --- | --- | --- |
| Café chairs facing incorrect directions | **Reproduced (validator rule on the generator's own data) and fixed** (S4-040) | `interior_chairs_before_after_seed8.png`: plan view of three interiors from the delivered and the fixed generator, each chair an arrow along its front, red when it faces more than 30 degrees away from its table (the delivered interior chairs are all exactly 90 degrees off); `alive_validate --seats SEED` prints the data; `tools/check_prop_orientation.py` checks the model's front axis on the real mesh |
| Vehicle hazard lights detached from the body | **Reproduced in a rendered frame, then fixed in three rendered steps** (S4-041): delivered (lamps on the side panel, on the roofline, in the air), then physics-box corners and bounding-box corners (a lamp still hung in the air), then mesh-surface anchors (lamps on the body, none in the air) | `hazard_lamps_three_versions_seed3_f3488.jpg` (the same frame, same camera, three renderers), `hazard_lamps_closeup_seed3_f3488_f3491.jpg`; tests in `tests/validate_test.cpp` |
| Uniform blue window materials | Not reproduced yet; physically based glass is on the roadmap (`docs/engineering/fidelity-plan.md`, addendum) | |
| Insufficient reflections, insufficient indirect lighting | Not reproduced as defects (they are missing features: hybrid reflections and GI are roadmap items P1) | |
| Low-quality or repetitive assets | Not reproduced yet; needs the asset library work (P3) | |

## What was looked at
By the author: the seating diagram, and the rendered hazard-lamp frames (seed 3, frames 3488 and 3491, 1280x720, Mesa llvmpipe, shadow map 2048, the licensed Car Concept) for the delivered renderer, the second step and the final one, from two cameras (the close-up and the wider street-side view), all with the same scout report. The camera is set by the diagnostic `SAIGE_ALIVE_FILM_CAMERA="ex,ey,ez,lx,ly,lz,fov"` (new, documented in `render/main.cpp`; the film's own camera is untouched). **No rendered frame of the chair fix was inspected** (the chair evidence is the plan-view diagram, built from the generator's own data, and the validator). The hosted full-asset visual step runs the renderer on the fixed code; its frames were not reviewed by a person, and the director has not looked at these images.

## Limits
* The seating rule judges chairs beside tables only and uses the chair model's front axis; other models (sofas, counters, display cases, stools) have their own front axes and are not yet checked. The interior generator's local-yaw convention (`yaw_facing_street`, local +Y) is likely wrong for every real glTF model; `sofa`, `counter` and `shelf` are the next to check.
* Semantic placement rules for furniture and street objects are started (seating) and not finished.
* The seating check was run on all four building fronts by rebuilding the interiors with every building turned to each leg (16 to 20 chairs per front); the generated districts alone only seat chairs on two fronts (legs 0 and 2), so before this check the frames of the other two were never exercised by data.
* Hazard lamps: added amber cubes on the body surface, not the car's own light meshes; one accident frame and one car model.
