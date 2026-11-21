#include "saige/render/camera.hpp"

namespace saige::render {

Camera look_at_camera(Float3 position, Float3 target, Float3 world_up, float fov_y_radians, float near_plane) noexcept
{
    Camera c;
    c.position = position;
    c.fov_y_radians = fov_y_radians;
    c.near_plane = near_plane;

    Float3 forward = normalize(target - position);
    if (forward == Float3{}) {
        forward = {1, 0, 0};
    }
    Float3 left = cross(normalize(world_up), forward);  // up x forward = left in the right-handed Z-up world
    if (length(left) < 1e-6f) {
        // Looking straight along the up axis: pick a stable left from the world axes.
        left = cross(std::fabs(forward.x) < 0.9f ? Float3{1, 0, 0} : Float3{0, 1, 0}, forward);
    }
    left = normalize(left);
    const Float3 up = cross(forward, left);
    c.rotation = from_basis(forward, left, up);
    return c;
}

Mat4 view_matrix(const Camera& c) noexcept
{
    const Float3 forward = camera_forward(c);
    const Float3 right = camera_right(c);
    const Float3 up = camera_up(c);
    Mat4 v = Mat4::identity();
    v.at(0, 0) = right.x;
    v.at(0, 1) = right.y;
    v.at(0, 2) = right.z;
    v.at(1, 0) = up.x;
    v.at(1, 1) = up.y;
    v.at(1, 2) = up.z;
    v.at(2, 0) = -forward.x;  // view space looks down -Z
    v.at(2, 1) = -forward.y;
    v.at(2, 2) = -forward.z;
    v.at(0, 3) = -dot(right, c.position);
    v.at(1, 3) = -dot(up, c.position);
    v.at(2, 3) = dot(forward, c.position);
    return v;
}

Mat4 projection_matrix(const Camera& c, float aspect) noexcept
{
    return perspective_reverse_z(c.fov_y_radians, aspect, c.near_plane);
}

Mat4 view_projection_matrix(const Camera& c, float aspect) noexcept
{
    return projection_matrix(c, aspect) * view_matrix(c);
}

namespace {

Plane make_plane(float a, float b, float cc, float d) noexcept
{
    const float len = std::sqrt(a * a + b * b + cc * cc);
    if (!(len > 1e-12f)) {
        return {{0, 0, 0}, 1.0f};  // degenerate: every point has distance 1 (always inside)
    }
    return {{a / len, b / len, cc / len}, d / len};
}

} // namespace

Frustum extract_frustum(const Mat4& vp) noexcept
{
    const auto row = [&vp](int r) { return Float4{vp.at(r, 0), vp.at(r, 1), vp.at(r, 2), vp.at(r, 3)}; };
    const Float4 x = row(0), y = row(1), z = row(2), w = row(3);
    // Inside: -w <= x <= w, -w <= y <= w, 0 <= z <= w (depth 1 = near, so z <= w is the near plane).
    Frustum f;
    f.planes[0] = make_plane(w.x + x.x, w.y + x.y, w.z + x.z, w.w + x.w);  // left
    f.planes[1] = make_plane(w.x - x.x, w.y - x.y, w.z - x.z, w.w - x.w);  // right
    f.planes[2] = make_plane(w.x + y.x, w.y + y.y, w.z + y.z, w.w + y.w);  // bottom
    f.planes[3] = make_plane(w.x - y.x, w.y - y.y, w.z - y.z, w.w - y.w);  // top
    f.planes[4] = make_plane(w.x - z.x, w.y - z.y, w.z - z.z, w.w - z.w);  // near
    f.planes[5] = make_plane(z.x, z.y, z.z, z.w);                          // far (infinite: degenerate, always inside)
    return f;
}

bool contains_point(const Frustum& f, Float3 p) noexcept
{
    for (const Plane& pl : f.planes) {
        if (pl.distance(p) < 0.0f) {
            return false;
        }
    }
    return true;
}

bool intersects_sphere(const Frustum& f, Float3 center, float radius) noexcept
{
    for (const Plane& pl : f.planes) {
        if (pl.distance(center) < -radius) {
            return false;
        }
    }
    return true;
}

bool intersects_aabb(const Frustum& f, Float3 min, Float3 max) noexcept
{
    for (const Plane& pl : f.planes) {
        const Float3 positive{pl.normal.x >= 0.0f ? max.x : min.x, pl.normal.y >= 0.0f ? max.y : min.y,
                              pl.normal.z >= 0.0f ? max.z : min.z};
        if (pl.distance(positive) < 0.0f) {
            return false;
        }
    }
    return true;
}

Ray pixel_ray(const Camera& c, float aspect, float ndc_x, float ndc_y) noexcept
{
    const float tan_half = std::tan(c.fov_y_radians * 0.5f);
    const Float3 direction = camera_forward(c) + camera_right(c) * (ndc_x * aspect * tan_half) +
                             camera_up(c) * (ndc_y * tan_half);
    return {c.position, normalize(direction)};
}

Projected project_point(const Mat4& vp, Float3 world) noexcept
{
    const Float4 clip = vp * Float4{world.x, world.y, world.z, 1.0f};
    Projected p;
    if (!(clip.w > 1e-9f)) {
        return p;
    }
    p.ndc_x = clip.x / clip.w;
    p.ndc_y = clip.y / clip.w;
    p.depth = clip.z / clip.w;
    p.in_front = true;
    return p;
}

} // namespace saige::render
