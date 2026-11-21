#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/math.hpp"

#include <cmath>

using namespace saige::render;

namespace {

bool near(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) <= eps;
}

Float4 transform(const Mat4& m, Float3 p)
{
    return m * Float4{p.x, p.y, p.z, 1.0f};
}

} // namespace

TEST_CASE("Math: column-major storage and multiplication order")
{
    Mat4 t = Mat4::identity();
    t.at(0, 3) = 5.0f;  // translation lives in the last column
    CHECK(t.m[12] == 5.0f);
    const Float4 p = transform(t, {1, 2, 3});
    CHECK(near(p.x, 6.0f));
    CHECK(near(p.w, 1.0f));

    Mat4 s = Mat4::identity();
    s.at(0, 0) = 2.0f;
    // (T * S) p = T(S p): scale first, then translate
    CHECK(near(transform(t * s, {1, 0, 0}).x, 7.0f));
    CHECK(near(transform(s * t, {1, 0, 0}).x, 12.0f));
}

TEST_CASE("Math: cross product follows the right-hand rule in the Z-up world")
{
    const Float3 forward{1, 0, 0};  // +X forward
    const Float3 left{0, 1, 0};     // +Y left
    CHECK(cross(forward, left) == Float3{0, 0, 1});  // forward x left = up
    CHECK(near(length(normalize({3, 4, 0})), 1.0f));
}

TEST_CASE("Math: look_at maps the target onto the -Z view axis with +Z world up as view +Y")
{
    const Float3 eye{-6, -4, 3};
    const Float3 target{0, 0, 0.5f};
    const Mat4 v = look_at(eye, target);

    const Float4 t = transform(v, target);
    CHECK(near(t.x, 0.0f, 1e-4f));
    CHECK(near(t.y, 0.0f, 1e-4f));
    CHECK(t.z < 0.0f);
    CHECK(near(-t.z, length(target - eye), 1e-4f));

    const Float4 e = transform(v, eye);
    CHECK(near(e.x, 0.0f) && near(e.y, 0.0f) && near(e.z, 0.0f));

    // A point above the target appears above it in view space.
    CHECK(transform(v, target + Float3{0, 0, 1}).y > 0.0f);
    // +Y (left) appears on the left of the screen when looking along +X.
    const Mat4 v2 = look_at({-5, 0, 0}, {0, 0, 0});
    CHECK(transform(v2, {0, 1, 0}).x < 0.0f);
}

TEST_CASE("Math: reverse-Z infinite projection depth range")
{
    const float near_plane = 0.1f;
    const Mat4 p = perspective_reverse_z(60.0f * pi / 180.0f, 16.0f / 9.0f, near_plane);

    const Float4 at_near = p * Float4{0, 0, -near_plane, 1};
    CHECK(near(at_near.z / at_near.w, 1.0f));

    const Float4 far_away = p * Float4{0, 0, -1.0e6f, 1};
    const float d = far_away.z / far_away.w;
    CHECK(d > 0.0f);
    CHECK(d < 1e-6f);

    const Float4 mid = p * Float4{0, 0, -10.0f, 1};
    CHECK(near(mid.z / mid.w, near_plane / 10.0f));

    // Points on the view axis project to the screen center, +Y view projects to +Y NDC (up).
    CHECK(near(mid.x / mid.w, 0.0f));
    const Float4 up = p * Float4{0, 1, -10.0f, 1};
    CHECK(up.y / up.w > 0.0f);
}
