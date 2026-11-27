# Changelog — SAIGE World Editor

## Build 012 (2026-11-26)
- Added `editor_assets.hpp/.cpp`: hash-indexed `AssetLibrary`, license/provenance tracking, duplicate hash & missing dependency detection, and `VehicleAssetMetadata` attachment validation (detecting detached hazard lights/wheels).
- Added `editor_semantic.hpp/.cpp`: `SemanticPlacementEngine` (yaw alignment, ground contact, walkway/clearance checks) and undoable `ApplySemanticPlacementCommand`.
- Added `editor_materials.hpp/.cpp`: `MaterialAuthoringAsset` with 12 PBR/glass presets, `GlassParameters`, and `InteriorMappingParameters` explicitly marked `PlannedEngineDependency`.
- Added `tests/editor_build012_test.cpp` (5 suites + 1k/10k entity & 10k asset benchmarks) while preserving all 10 `editor_core_test.cpp` suites.

## Build 011
- Scene hierarchy, transactional `CommandHistory`, `SED1` v1 serializer, `IWorldValidatorBridge`, `ViewportState`, and `WorldDataExporter`.
