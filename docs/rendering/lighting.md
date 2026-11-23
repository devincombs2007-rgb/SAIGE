# Scene lighting for imported models

`RendererSettings::lighting` (a `SceneLighting`) sets the sun and the ground ambient used to light imported glTF models:

| Field | Meaning | Default |
| --- | --- | --- |
| `sun_direction` | points toward the sun; any nonzero length, normalized on use | `(-0.40, -0.55, 0.73)` |
| `sun_color` | linear radiance scale of the sun; may exceed 1 when `RendererSettings::hdr` is on | `(1.00, 0.95, 0.85)` |
| `ambient_ground` | ambient light arriving from below | `(0.20, 0.18, 0.16)` |

The sky half of the hemispherical ambient is 0.6 times `RendererSettings::sky`, which is also the fog color, so changing the sky
changes the ambient and the fog together. The defaults are the values that were fixed in the constructor before this setting, so
nothing changes unless a host sets it (a test compares the explicit defaults with an unset renderer byte for byte).

Invalid lighting (a non-finite component, a negative color, or a zero sun direction) throws `std::invalid_argument` from the
`SceneRenderer` constructor, before any GPU object exists.

Scope: the setting lights imported models in every shading mode. The 0.1 cube and ground shaders keep their own fixed lighting. There
are no shadows and no image-based lighting yet (milestones M5 and M6); this is one directional light and a hemispherical ambient.
