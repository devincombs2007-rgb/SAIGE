# Graphical Integration Specification for Claude (Per-Frame Render State)

This specification defines the exact simulation state extracted at the end of `IGame::fixed_update` (`16.667 ms`) in ascending entity ID order (`docs/architecture.md` snapshot contract) for rendering via `IFrameSink`:

## 1. Pedestrian / NPC Render Instances (per `NpcAgent` + `CrowdAgent`)
- `EntityId id`: Stable entity ID for mesh/skeleton/animation state lookup.
- `WorldPosition position`: Z-up X/Y position (world `x, y` from `CrowdAgent::position`, `z` from `MultiFloorNavGraph` slice elevation).
- `Vec2d velocity`: Horizontal velocity for facing yaw (`atan2(vy, vx)`) and locomotion blend speed (`|v|` -> idle / walk / flee sprint).
- `NpcIntent intent`: Drives upper-body/stance state (`Idle`, `Rest`, `Work`, `Shop`, `Socialize`, `Investigate`, `Flee`).
- `SimLodTier lod`: `Active` (full skeletal + IK), `Background` (simplified animation/impostor), `Dormant` (culled).

## 2. Vehicle Render Instances (per `TrafficVehicle` + `src/vehicle.cpp`)
- `VehicleId id` & `VehicleRole role` (`Civilian`, `Commercial`, `Emergency`).
- Authoritative chassis transform (`WorldPosition`, yaw, pitch, roll) & wheel spin/steer angles from `src/vehicle.cpp`.
- `VehicleControlInput::brake > 0.05` -> illuminate brake tail lights.
- `VehicleControlInput::siren_active` -> enable emergency lightbar strobes & spatial audio siren voice.

## 3. Traffic Signal & Crosswalk State
- Per `IntersectionController`: `active_phase_idx`, `SignalColor` (`Red`, `Yellow`, `Green`) per approach lane for emissive signal head materials.
- Per `CrosswalkZone`: `pedestrian_present` boolean (optional debug overlay / walk-don't-walk indicator).

## 4. World Events & Multi-Floor Debug Overlays
- Active `WorldEventRecord` list (`id`, `position`, `radius_m`, `kind`) for hazard VFX/audio emitters.
- Optional debug draw: `MultiFloorPathResult::waypoints` (`gx, gy, z_m`) and `DoorwayPortal` throat bounds.
