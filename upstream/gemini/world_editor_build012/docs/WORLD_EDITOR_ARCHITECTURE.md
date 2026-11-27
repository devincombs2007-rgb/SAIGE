# SAIGE World Editor — Architecture (Build 012)

## 1. Core Modules
- **`editor_scene.hpp/.cpp`:** Right-handed Z-up meter-based `EditorScene`, stable `EntityId` allocation, parent-child hierarchy with cycle prevention, `64.0 m` chunk indexing (`ChunkCoord`), `SelectionSet`, `SnapSettings`, and `validate()`.
- **`editor_commands.hpp/.cpp`:** Transactional `CommandHistory` (`begin_transaction`, `commit_transaction`, `rollback_transaction`) with `CreateNodeCommand`, `DeleteSubtreeCommand`, `SetTransformCommand`, `ReparentCommand`, `DuplicateSubtreeCommand`, and `GroupNodesCommand`.
- **`editor_project.hpp/.cpp`:** Binary `SED1` v1 serializer (`EditorProjectSerializer`) with bounded string/node limits and corruption rejection.
- **`editor_workspace.hpp/.cpp`:** `IWorldValidatorBridge` (authoritative Claude-owned engine validation adapter), `ViewportState`, and `WorldDataExporter` (Z-up JSON).
- **`editor_assets.hpp/.cpp` (New in Build 012):** Hash-indexed `AssetLibrary` supporting categories, tags, collections, favorites, SPDX license & source URL provenance, dependency tracking, duplicate content-hash detection, quality validation, and `VehicleAssetMetadata` attachment checks (`DETACHED_VEHICLE_LIGHT`, `DETACHED_VEHICLE_WHEEL`).
- **`editor_semantic.hpp/.cpp` (New in Build 012):** `SemanticPlacementEngine` (`propose_alignment`, `validate_clearance`) and reversible `ApplySemanticPlacementCommand` integrated with `CommandHistory`.
- **`editor_materials.hpp/.cpp` (New in Build 012):** `MaterialAuthoringAsset` with 12 physical presets (`AutomotivePaint`, `ClearGlass`, `TintedGlass`, `FrostedGlass`, `ReflectiveArchitecturalGlass`, `WetAsphalt`, etc.), `GlassParameters`, and `InteriorMappingParameters` explicitly tagged with `FeatureSupportStatus::PlannedEngineDependency`.
