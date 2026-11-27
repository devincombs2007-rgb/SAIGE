# World Editor Build 012: integration and the review 7198676267 fixes (2026-11-27)

Received in the Gemini engineering transfer (`upstream/gemini/PROVENANCE.md`; the delivered files are unchanged in `upstream/gemini/world_editor_build012/`).
Owner directive 7209812710 makes Claude the maintainer. No editor code existed in the repository before.

## What is integrated
`engine/editor/`: the library `saige_editor` (scene, commands with undo and redo, project persistence `SED1`, workspace, asset library, semantic placement,
material authoring) built with `-Wall -Wextra -Wpedantic -Werror -Wconversion -Wsign-conversion -Wshadow`, no renderer or window-system dependency, and three
test programs registered with CTest: the delivered `editor_core_test` (10 suites) and `editor_build012_test` (5 suites), and `editor_regression_test`
(53 checks) for the review findings. All three also pass under AddressSanitizer and UndefinedBehaviorSanitizer.

## Review 7198676267, item by item (each reproduced on the delivered code with a probe first)
| Finding | Delivered behaviour (measured) | Now |
| --- | --- | --- |
| Invalid glass opacity, transmission, thickness | opacity 5, transmission -1, thickness -0.01: one diagnostic (the opaque-glass warning), no range error | range errors for opacity, transmission, roughness, tint, thickness (0, 1] m, absorption; base colour and emissive checked too |
| NaN room dimension | 0 diagnostics (`NaN <= 0.5` is false) | non-finite, <= 0.5 m or > 100 m is an error; day/night blend and fade distance checked |
| Cross-chunk clearance | objects 0.2 m apart at x = 63.9 and 64.1: 0 diagnostics (only the node's own chunk was searched) | world-space comparison across chunk boundaries |
| Local rather than world transforms | a child at world x = 11 next to an object at x = 11: 0 diagnostics | `EditorScene::world_position` composes every ancestor (scale, rotation R = Rz Ry Rx, translation); clearance and grounding use it; a node is not compared with its own ancestors or descendants |
| Z = 0 ground assumption | grounding compared with z = 0 | `validate_clearance(..., ground_z_m)` |
| Unused minimum walkway clearance | stored, never read | `WALKWAY_NARROW` (Info) when the gap beyond the clearance radius is narrower than the walkway |
| +Y front instead of the engine's +X | a chair facing a table along +X got yaw -90 | `SemanticMetadata::local_front_dir` defaults to +X; placement turns the authored front axis toward the anchor, so an asset authored with +Y says so and is turned correctly (tested both ways). The delivered test's +90 expectation becomes 180 under the +X convention; the change is commented in `engine/editor/tests/editor_build012_test.cpp` |
| Asset validation passed bogus provenance and vehicle data | a record with licence `TOTALLY-FREE`, URL `not a url`, no author, a self dependency, wheel radius -0.3 and a seat 9 m above the body: **passed, 0 errors** | `UNRECOGNISED_LICENSE` (allow-list of reusable SPDX ids; NC/ND refused), `INVALID_SOURCE_URL`, `MISSING_AUTHOR`, `SELF_DEPENDENCY`, `INVALID_WHEEL_RADIUS`, `DETACHED_VEHICLE_SEAT`; `provenance_verified` separates declared from verified provenance (`PROVENANCE_UNVERIFIED`, Info) |
| No demonstrated persistence of new metadata | semantic metadata was never saved | `store_semantic_metadata` / `load_semantic_metadata` keep it as versioned node properties (`semantic.v1.*`) that the existing `SED1` format already persists; validated on store and on load; a save and load round trip is tested |

Mutation: 6 of 6 mutants of these fixes are caught by `editor_regression_test` (parents ignored in world positions, the old +Y yaw, the licence allow-list
removed, the ground height ignored, NaN rooms passing, seat checks disabled).

## Not done
- **Runtime validator bridge**: Build 012 names an `IWorldValidatorBridge` owned by Claude but delivers no interface. The district validator on
  `claude/s04-quality` (`district_validate`, the SEAT_FACING and hazard-lamp rules) is the natural implementation; the contract is not written yet.
- **Material authoring persistence**: `MaterialAuthoringAsset` is validated but not saved by the project format.
- **Glass refraction and interior-mapping shaders** stay renderer work (`FeatureSupportStatus::PlannedEngineDependency` is accurate).
- No editor UI; this is the editing core.
