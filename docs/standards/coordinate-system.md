# SAIGE Engine Standard: Coordinate System & Units

**Status:** ✅ **Approved engine-wide standard.** Decided by the project owner on 2026-11-21 (decision D-06, issue #2).
**Applies to:** every SAIGE subsystem: core, gameplay, physics, animation, audio, rendering, tools and importers.

## 1. World coordinate system

| Property | Standard |
|----------|----------|
| Handedness | **Right-handed** |
| Up axis | **+Z** |
| Length unit | **1 unit = 1 meter** |
| Forward / left axes | **+X forward, +Y left, +Z up** (accepted by Core in #2, see §5) |

```
        +Z (up)
         │
         │
         │
         └──────── +Y
        ╱
       ╱
     +X
```

Right-handed means `X × Y = Z`. Positive rotation about an axis is counter-clockwise when looking
from the positive end of that axis toward the origin.

## 2. Other units

| Quantity | Unit |
|----------|------|
| Length | meters (m) |
| Mass | kilograms (kg) |
| Time | seconds (s), stored as `double` for absolute time and `float` for deltas |
| Angles in code and data | **radians**. Degrees only in UI and editor display. |
| Light (rendering) | Physical units: lux (directional), lumens or candela (punctual), nits (emissive/sky luminance), EV100 for camera exposure |
| Color | Linear Rec.709 primaries in shading. sRGB transfer function only for 8-bit color textures and display output. |

## 3. Importing third-party content

Every importer converts to the engine standard **at import time**. Runtime code never handles
foreign axis conventions.

| Source | Native convention | Conversion |
|--------|-------------------|------------|
| glTF 2.0 | Right-handed, **Y-up**, meters, model front faces +Z | Rotate +90° about X: `(x, y, z) → (x, −z, y)`. This is a proper rotation (det = +1), so **triangle winding is preserved**. An extra yaw may be applied once the forward axis (§5) is confirmed. |
| FBX | Varies per file (stored in header) | Read the axis system and unit scale from the file and convert (future importer) |
| Blender (via glTF export) | Z-up internally; the glTF exporter writes Y-up | Same as glTF |

Importers must also convert **units** (e.g., centimeters → meters) and record the original source
convention in asset metadata, for traceability.

## 4. Matrices & transforms (pending D-07 confirmation by Core)

Recommended convention, to be confirmed by Core when the math library lands:
- Column vectors, transforms compose right-to-left: `p_world = M_world * M_local * p`.
- Column-major storage, so matrices upload to HLSL `column_major` without transposing.
- Quaternions stored as `(x, y, z, w)`.

## 5. Forward axis (decided)

**Decision:** +X forward, +Y left, +Z up. Core accepted this in issue #2 and `docs/core-rendering-contract.md`.
- It's right-handed with `forward × left = up`, which matches the robotics (REP-103) and several
  game-engine conventions.
- Yaw is a positive rotation about +Z, measured from +X toward +Y, so it equals `atan2(y, x)`. That
  makes heading math straightforward for vehicles, AI navigation and gameplay.

Importers apply the Y-up → Z-up rotation and then a yaw that maps the source asset's front onto +X.

## 6. Rendering-internal conventions (not engine-wide)

These are internal to the renderer, documented here so nobody is surprised by them:
- **Reverse-Z** depth: `D32_SFLOAT`, near plane maps to 1, far plane to 0, infinite far plane.
- Vulkan clip space is Y-down. The renderer flips it with a negative-height viewport, so world, view
  and NDC math stays conventional.
- View space follows the camera's own local axes in the world convention above. The renderer derives
  projection matrices internally.
