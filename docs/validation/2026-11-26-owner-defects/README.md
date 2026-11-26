# Owner-reported defects: first reproductions (2026-11-26)

Branch `claude/s04-quality`. The film is not touched; these are corrections to the world generator and the renderer's placement code, with validators.

| Owner report | Status | Evidence |
| --- | --- | --- |
| Café chairs facing incorrect directions | **Reproduced (validator rule on the generator's own data) and fixed** (S4-040) | `interior_chairs_before_after_seed8.png`: plan view of three interiors from the delivered and the fixed generator, each chair an arrow along its front, red when it faces more than 30 degrees away from its table (the delivered interior chairs are all exactly 90 degrees off); `alive_validate --seats SEED` prints the data; `tools/check_prop_orientation.py` checks the model's front axis on the real mesh |
| Vehicle hazard lights detached from the body | **Reproduced by reading and a placement test, and the placement fixed** (S4-041); not yet seen in a rendered frame; the lamps are placed from the physics box, not the visible mesh, so a small offset from the bumper may remain | `test_hazard_lamps` in `tests/validate_test.cpp` |
| Uniform blue window materials | Not reproduced yet; physically based glass is on the roadmap (`docs/engineering/fidelity-plan.md`, addendum) | |
| Insufficient reflections, insufficient indirect lighting | Not reproduced as defects (they are missing features: hybrid reflections and GI are roadmap items P1) | |
| Low-quality or repetitive assets | Not reproduced yet; needs the asset library work (P3) | |

## What was looked at
The diagram was looked at by the author. **No rendered frame of either fix was inspected**: the renderer needs the restored assets and a GPU or software Vulkan device, and the fixes are in placement data, which the diagram and tests cover directly. The hosted full-asset visual step runs the renderer on the fixed generator; its frames were not reviewed by a person.

## Limits
* The seating rule judges chairs beside tables only and uses the chair model's front axis; other models (sofas, counters, display cases, stools) have their own front axes and are not yet checked. The interior generator's local-yaw convention (`yaw_facing_street`, local +Y) is likely wrong for every real glTF model; `sofa`, `counter` and `shelf` are the next to check.
* Semantic placement rules for furniture and street objects are started (seating) and not finished.
