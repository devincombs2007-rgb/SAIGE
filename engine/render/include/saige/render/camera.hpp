// SAIGE Render — perspective camera, projection and frustum.
//
// Conventions (docs/standards/coordinate-system.md, docs/rendering/0.2-architecture.md §3.1):
// - A camera's local axes follow the engine's world axes: it looks along local +X (forward), local +Y is left, local
//   +Z is up. An identity rotation therefore looks along world +X with +Z up.
// - View space is right-handed with the camera looking down -Z, +Y up, +X right (what shaders and look_at expect).
// - Projection is reverse-Z with an infinite far plane: depth 1 at the near plane tending to 0 at infinity, NDC +Y up.
//   There is deliberately no far-plane parameter.
#pragma once

#include "saige/render/math.hpp"
#include "saige/render/transform.hpp"

#include <array>
#include <optional>

namespace saige::render {

struct Camera {
    Float3 position;
    Quat rotation;  ///< Orientation; identity looks along +X with +Z up.
    float fov_y_radians = 60.0f * pi / 180.0f;
    float near_plane = 0.1f;
};

[[nodiscard]] inline Float3 camera_forward(const Camera& c) noexcept { return rotate(normalize(c.rotation), {1, 0, 0}); }
[[nodiscard]] inline Float3 camera_left(const Camera& c) noexcept { return rotate(normalize(c.rotation), {0, 1, 0}); }
[[nodiscard]] inline Float3 camera_up(const Camera& c) noexcept { return rotate(normalize(c.rotation), {0, 0, 1}); }
[[nodiscard]] inline Float3 camera_right(const Camera& c) noexcept { return camera_left(c) * -1.0f; }

/// Camera at `position` looking at `target`. `world_up` must not be parallel to the view direction; if it is (or
/// position == target) the result falls back to a stable orientation instead of producing NaN.
[[nodiscard]] Camera look_at_camera(Float3 position, Float3 target, Float3 world_up = {0.0f, 0.0f, 1.0f},
                                    float fov_y_radians = 60.0f * pi / 180.0f, float near_plane = 0.1f) noexcept;

/// World -> view.
[[nodiscard]] Mat4 view_matrix(const Camera& c) noexcept;
/// View -> clip for the given aspect ratio (width / height), reverse-Z infinite.
[[nodiscard]] Mat4 projection_matrix(const Camera& c, float aspect) noexcept;
/// World -> clip.
[[nodiscard]] Mat4 view_projection_matrix(const Camera& c, float aspect) noexcept;

/// Plane n . p + d = 0 with a unit normal pointing into the frustum: points with n . p + d >= 0 are inside.
struct Plane {
    Float3 normal;
    float d = 0.0f;
    [[nodiscard]] float distance(Float3 p) const noexcept { return dot(normal, p) + d; }
};

/// View frustum as planes in world space: left, right, bottom, top, near. With an infinite far plane there is no far
/// plane; `far` is always inside (a degenerate plane) so callers can iterate all six uniformly.
struct Frustum {
    std::array<Plane, 6> planes;  ///< left, right, bottom, top, near, far
};

/// Extracts the frustum from a world -> clip matrix built for reverse-Z infinite projection (Gribb-Hartmann).
[[nodiscard]] Frustum extract_frustum(const Mat4& view_projection) noexcept;

[[nodiscard]] bool contains_point(const Frustum& f, Float3 p) noexcept;
/// Conservative: may report a sphere near a frustum corner as visible, never reports a visible sphere as culled.
[[nodiscard]] bool intersects_sphere(const Frustum& f, Float3 center, float radius) noexcept;
/// Conservative axis-aligned box test (positive-vertex method).
[[nodiscard]] bool intersects_aabb(const Frustum& f, Float3 min, Float3 max) noexcept;

struct Ray {
    Float3 origin;
    Float3 direction;  ///< unit length
};

/// World-space ray through normalized device coordinates (x right, y up, both in [-1, 1]).
[[nodiscard]] Ray pixel_ray(const Camera& c, float aspect, float ndc_x, float ndc_y) noexcept;

struct Projected {
    float ndc_x = 0.0f;
    float ndc_y = 0.0f;
    float depth = 0.0f;  ///< reverse-Z depth in (0, 1], 1 at the near plane
    bool in_front = false;
};

/// Projects a world point; `in_front` is false at or behind the camera plane (the other fields are then meaningless).
[[nodiscard]] Projected project_point(const Mat4& view_projection, Float3 world) noexcept;

/// NDC to pixel coordinates (origin top-left, y down) for a width x height target.
struct PixelPos {
    float x = 0.0f;
    float y = 0.0f;
};
[[nodiscard]] constexpr PixelPos ndc_to_pixel(float ndc_x, float ndc_y, float width, float height) noexcept
{
    return {(ndc_x * 0.5f + 0.5f) * width, (0.5f - ndc_y * 0.5f) * height};
}

} // namespace saige::render
