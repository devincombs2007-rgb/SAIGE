#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/camera.hpp"

#include <cmath>
#include <limits>

using namespace saige::render;

namespace {

bool near(float a, float b, float eps = 1e-4f)
{
    return std::fabs(a - b) <= eps;
}

bool near(Float3 a, Float3 b, float eps = 1e-4f)
{
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

constexpr float aspect_16_9 = 16.0f / 9.0f;
constexpr float half_pi = pi * 0.5f;

} // namespace

TEST_CASE("Camera: identity looks along +X with +Z up and +Y to the left")
{
    const Camera c;
    CHECK(near(camera_forward(c), {1, 0, 0}));
    CHECK(near(camera_left(c), {0, 1, 0}));
    CHECK(near(camera_up(c), {0, 0, 1}));
    CHECK(near(camera_right(c), {0, -1, 0}));
    // Right-handed: forward x left = up
    CHECK(near(cross(camera_forward(c), camera_left(c)), camera_up(c)));
}

TEST_CASE("Camera: view matrix sends the forward axis to -Z and the camera position to the origin")
{
    Camera c;
    c.position = {2, -3, 1.5f};
    c.rotation = from_axis_angle({0, 0, 1}, 0.6f) * from_axis_angle({0, 1, 0}, 0.2f);
    const Mat4 v = view_matrix(c);
    CHECK(near(transform_point(v, c.position), {0, 0, 0}));
    CHECK(near(transform_point(v, c.position + camera_forward(c) * 5.0f), {0, 0, -5}));
    CHECK(near(transform_point(v, c.position + camera_up(c) * 2.0f), {0, 2, 0}));
    CHECK(near(transform_point(v, c.position + camera_right(c) * 3.0f), {3, 0, 0}));
    // A rigid transform: view * inverse(view) = identity and the upper 3x3 is orthonormal.
    const auto inv = inverse(v);
    REQUIRE(inv.has_value());
    CHECK(near(transform_point(*inv, {0, 0, 0}), c.position));
}

TEST_CASE("Camera: look_at_camera aims at the target with +Z up (no roll)")
{
    const Camera c = look_at_camera({-6, -4.5f, 4}, {0, 0, 0.5f});
    const Float3 expected = normalize(Float3{0, 0, 0.5f} - c.position);
    CHECK(near(camera_forward(c), expected, 1e-5f));
    CHECK(near(camera_up(c).x * camera_forward(c).x + camera_up(c).y * camera_forward(c).y +
                   camera_up(c).z * camera_forward(c).z,
               0.0f, 1e-5f));                    // up is perpendicular to forward
    CHECK(camera_up(c).z > 0.0f);                // never upside down
    CHECK(near(camera_left(c).z, 0.0f, 1e-5f));  // no roll: the left axis stays horizontal
    // Agrees with math.hpp's look_at.
    const Mat4 a = view_matrix(c);
    const Mat4 b = look_at(c.position, {0, 0, 0.5f});
    for (int i = 0; i < 16; ++i) {
        CHECK(near(a.m[i], b.m[i], 1e-4f));
    }
}

TEST_CASE("Camera: degenerate look_at inputs stay finite")
{
    const Camera same = look_at_camera({1, 1, 1}, {1, 1, 1});          // position == target
    CHECK(std::isfinite(same.rotation.w));
    CHECK(near(camera_forward(same), {1, 0, 0}));
    const Camera down = look_at_camera({0, 0, 5}, {0, 0, 0});          // straight down: up is parallel to forward
    CHECK(near(camera_forward(down), {0, 0, -1}));
    CHECK(std::isfinite(camera_left(down).x));
    CHECK(near(length(camera_left(down)), 1.0f));
    const Camera up = look_at_camera({0, 0, 0}, {0, 0, 5});
    CHECK(near(camera_forward(up), {0, 0, 1}));
}

TEST_CASE("Camera: points project where the geometry says (center, edges, behind)")
{
    Camera c;  // at the origin looking along +X
    c.fov_y_radians = half_pi;  // 90 degrees vertical
    const Mat4 vp = view_projection_matrix(c, aspect_16_9);

    const Projected center = project_point(vp, {10, 0, 0});
    REQUIRE(center.in_front);
    CHECK(near(center.ndc_x, 0.0f));
    CHECK(near(center.ndc_y, 0.0f));

    // 90 degree vertical FOV: a point 10 m ahead and 10 m up is exactly at the top edge (ndc_y = 1).
    CHECK(near(project_point(vp, {10, 0, 10}).ndc_y, 1.0f));
    // ... and 10 m to the left is at ndc_x = -1/aspect... left of screen center means negative x.
    const Projected left = project_point(vp, {10, 10, 0});
    CHECK(left.ndc_x < 0.0f);
    CHECK(near(left.ndc_x, -1.0f / aspect_16_9));
    CHECK(project_point(vp, {10, -10, 0}).ndc_x > 0.0f);  // right of center

    CHECK_FALSE(project_point(vp, {-5, 0, 0}).in_front);   // behind the camera
    CHECK_FALSE(project_point(vp, {0, 0, 0}).in_front);    // on the camera plane
}

TEST_CASE("Camera: reverse-Z depth is 1 at the near plane and tends to 0 with distance")
{
    Camera c;
    c.near_plane = 0.5f;
    const Mat4 vp = view_projection_matrix(c, aspect_16_9);
    CHECK(near(project_point(vp, {0.5f, 0, 0}).depth, 1.0f));
    CHECK(near(project_point(vp, {5.0f, 0, 0}).depth, 0.1f));
    const float far_depth = project_point(vp, {1.0e6f, 0, 0}).depth;
    CHECK(far_depth > 0.0f);
    CHECK(far_depth < 1e-5f);
    // Strictly monotonic: farther is smaller (so the depth test is Greater).
    CHECK(project_point(vp, {2, 0, 0}).depth > project_point(vp, {3, 0, 0}).depth);
}

TEST_CASE("Camera: frustum planes classify points like the projection does")
{
    Camera c = look_at_camera({0, 0, 1}, {10, 0, 1});
    const Mat4 vp = view_projection_matrix(c, aspect_16_9);
    const Frustum f = extract_frustum(vp);

    // Plane normals are unit length and point inward (the center ray's point is inside every plane).
    for (int i = 0; i < 5; ++i) {
        CHECK(near(length(f.planes[static_cast<std::size_t>(i)].normal), 1.0f, 1e-4f));
    }
    CHECK(contains_point(f, {5, 0, 1}));
    CHECK_FALSE(contains_point(f, {-1, 0, 1}));      // behind: outside the near plane
    CHECK_FALSE(contains_point(f, {0.05f, 0, 1}));   // between the camera and the near plane (0.1)
    CHECK(contains_point(f, {1.0e5f, 0, 1}));        // infinitely far is still inside (no far plane)
    CHECK_FALSE(contains_point(f, {5, 0, 100}));     // far above the view
    CHECK_FALSE(contains_point(f, {5, 100, 1}));     // far to the left
    CHECK_FALSE(contains_point(f, {5, -100, 1}));    // far to the right

    // contains_point must agree with a projection-based visibility test for many sample points.
    int agree = 0;
    int total = 0;
    for (float x = -2.0f; x <= 20.0f; x += 1.7f) {
        for (float y = -15.0f; y <= 15.0f; y += 2.3f) {
            for (float z = -8.0f; z <= 10.0f; z += 2.1f) {
                const Float3 p{x, y, z};
                const Projected pr = project_point(vp, p);
                const bool visible = pr.in_front && std::fabs(pr.ndc_x) <= 1.0f && std::fabs(pr.ndc_y) <= 1.0f &&
                                     pr.depth <= 1.0f;
                ++total;
                agree += visible == contains_point(f, p) ? 1 : 0;
            }
        }
    }
    CHECK(total > 500);
    CHECK(agree == total);
}

TEST_CASE("Camera: sphere and box culling are conservative and correct at the boundaries")
{
    const Camera c = look_at_camera({0, 0, 0}, {1, 0, 0});
    const Frustum f = extract_frustum(view_projection_matrix(c, 1.0f));  // 60 degree vertical FOV, square

    CHECK(intersects_sphere(f, {10, 0, 0}, 0.5f));
    CHECK_FALSE(intersects_sphere(f, {-10, 0, 0}, 0.5f));         // behind
    CHECK(intersects_sphere(f, {-0.3f, 0, 0}, 0.5f));             // straddles the near plane
    CHECK_FALSE(intersects_sphere(f, {10, 100, 0}, 1.0f));        // far outside the left plane
    // A sphere well outside the left plane but with a huge radius reaches back in.
    CHECK(intersects_sphere(f, {10, 100, 0}, 200.0f));

    CHECK(intersects_aabb(f, {9, -1, -1}, {11, 1, 1}));
    CHECK_FALSE(intersects_aabb(f, {-11, -1, -1}, {-9, 1, 1}));
    CHECK(intersects_aabb(f, {-1, -1, -1}, {1, 1, 1}));           // contains the camera
    CHECK_FALSE(intersects_aabb(f, {9, 50, -1}, {11, 52, 1}));
    CHECK(intersects_aabb(f, {9, 3, -1}, {11, 5, 1}));            // 60 degree FOV, square: half-width at x=10 is ~5.77
}

TEST_CASE("Camera: pixel rays pass through the point that projects to that pixel")
{
    Camera c = look_at_camera({-3, 2, 4}, {5, -1, 0.5f}, {0, 0, 1}, 70.0f * pi / 180.0f, 0.2f);
    const Mat4 vp = view_projection_matrix(c, aspect_16_9);

    for (const auto& [nx, ny] : {std::pair{0.0f, 0.0f}, std::pair{0.6f, -0.3f}, std::pair{-0.9f, 0.8f},
                                std::pair{1.0f, 1.0f}}) {
        const Ray ray = pixel_ray(c, aspect_16_9, nx, ny);
        CHECK(near(length(ray.direction), 1.0f));
        const Float3 world = ray.origin + ray.direction * 12.5f;
        const Projected p = project_point(vp, world);
        REQUIRE(p.in_front);
        CHECK(near(p.ndc_x, nx, 1e-3f));
        CHECK(near(p.ndc_y, ny, 1e-3f));
    }
    // The center pixel looks along the camera's forward axis.
    CHECK(near(pixel_ray(c, aspect_16_9, 0, 0).direction, camera_forward(c), 1e-5f));
}

TEST_CASE("Camera: NDC to pixel conversion has its origin at the top-left")
{
    const PixelPos tl = ndc_to_pixel(-1.0f, 1.0f, 320.0f, 180.0f);
    CHECK(near(tl.x, 0.0f));
    CHECK(near(tl.y, 0.0f));
    const PixelPos br = ndc_to_pixel(1.0f, -1.0f, 320.0f, 180.0f);
    CHECK(near(br.x, 320.0f));
    CHECK(near(br.y, 180.0f));
    const PixelPos mid = ndc_to_pixel(0.0f, 0.0f, 320.0f, 180.0f);
    CHECK(near(mid.x, 160.0f));
    CHECK(near(mid.y, 90.0f));
}
