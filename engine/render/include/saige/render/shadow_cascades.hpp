// Cascaded shadow map fitting for one directional light: pure CPU math, no GPU objects, so it can be verified exhaustively before any
// shadow pass exists (milestone M6 of docs/rendering/material-architecture.md; research in docs/rendering/m5-m6-research.md).
//
// The camera's view range [near, shadow_distance] is split into up to four slices (practical split scheme: a blend of uniform and
// logarithmic splits). Each slice gets an orthographic light-space projection fitted to the bounding *sphere* of the slice, which makes
// the cascade's size independent of the camera's orientation, and the sphere center is snapped to whole shadow-map texels in light
// space, so a world point lands on the same fractional texel position whatever the camera does: shadow edges do not shimmer when the
// camera moves or turns. Selecting a cascade at shading time uses view-space depth, the same measure the alpha-blending sort uses.
//
// Conventions: right-handed, +Z up, meters. `to_sun` is the unit vector from the scene toward the sun (the direction the sun's light
// comes *from*). The light projection maps x and y to [-1, 1] and depth to [0, 1] with 1 at the light-side near plane (reverse-Z, like
// the camera), so a depth test of `greater` keeps the occluder nearest the light.
#pragma once

#include "saige/render/math.hpp"

#include <array>
#include <cstdint>

namespace saige::render {

inline constexpr uint32_t kMaxShadowCascades = 4;

struct CascadeSettings {
    uint32_t count = 4;            ///< 1 to kMaxShadowCascades
    float near_plane = 0.1f;       ///< the camera's near plane; the first slice starts here
    float shadow_distance = 150.0f;  ///< the last slice ends here (view-space depth, meters); beyond it there is no shadow
    float split_lambda = 0.75f;    ///< 0 = uniform splits, 1 = logarithmic
    uint32_t resolution = 2048;    ///< shadow map size per cascade (texels), for the texel snapping
    float caster_margin = 50.0f;   ///< how far behind the slice, toward the sun, the light's near plane is pulled (off-screen casters)
    bool stabilize = true;         ///< snap the cascade to whole texels (turn off only to see the shimmer it removes)
};

/// What fitting needs to know about the camera: where it is, where it looks, and its perspective shape. The roll about `forward` does
/// not matter (the sphere fit is rotation invariant).
struct CascadeCamera {
    Float3 position;
    Float3 forward;  ///< unit
    float fov_y_radians = 1.0471976f;
    float aspect = 16.0f / 9.0f;
};

struct ShadowCascade {
    Mat4 view_proj;          ///< world to light clip space
    Float3 center;           ///< the (snapped) world-space center of the cascade's bounding sphere
    float radius = 0.0f;     ///< bounding-sphere radius of the slice, meters
    float split_near = 0.0f; ///< view-space depth where the slice starts
    float split_far = 0.0f;  ///< and ends
    float texel_world_size = 0.0f;  ///< meters per shadow-map texel at this cascade
    float depth_range = 0.0f;       ///< meters spanned by the light-space depth range [0, 1]
};

struct ShadowCascades {
    std::array<ShadowCascade, kMaxShadowCascades> cascades{};
    uint32_t count = 0;  ///< 0 when the settings or camera are invalid
};

/// Split depths: result[0] = near, result[count] = far, strictly increasing. `lambda` blends uniform (0) and logarithmic (1) splits.
/// Returns zeros past `count`; invalid input (count 0 or above the maximum, near <= 0, far <= near) yields all zeros.
[[nodiscard]] std::array<float, kMaxShadowCascades + 1> cascade_splits(float near_plane, float far_plane, uint32_t count, float lambda) noexcept;

/// Center (as a distance along the view axis from the camera) and radius of the smallest sphere through the corners of the slice of a
/// perspective frustum between view depths `slice_near` and `slice_far`.
struct SliceSphere {
    float axis_distance = 0.0f;
    float radius = 0.0f;
};
[[nodiscard]] SliceSphere slice_bounding_sphere(float slice_near, float slice_far, float fov_y_radians, float aspect) noexcept;

/// Fits the cascades. Deterministic; the light's basis depends only on `to_sun`. Returns count 0 for invalid input.
[[nodiscard]] ShadowCascades fit_shadow_cascades(const CascadeCamera& camera, Float3 to_sun, const CascadeSettings& settings) noexcept;

/// The cascade a fragment at view-space `depth` uses: the first whose far split exceeds it, or `count` (no shadow) beyond the last.
[[nodiscard]] uint32_t select_cascade(const ShadowCascades& cascades, float depth) noexcept;

} // namespace saige::render
