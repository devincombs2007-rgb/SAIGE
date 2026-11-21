#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/transform.hpp"

#include <cmath>
#include <limits>

using namespace saige::render;

namespace {

bool near(float a, float b, float eps = 1e-5f)
{
    return std::fabs(a - b) <= eps;
}

bool near(Float3 a, Float3 b, float eps = 1e-5f)
{
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}

bool near(const Mat4& a, const Mat4& b, float eps = 1e-4f)
{
    for (int i = 0; i < 16; ++i) {
        if (!near(a.m[i], b.m[i], eps)) {
            return false;
        }
    }
    return true;
}

constexpr float half_pi = pi * 0.5f;

} // namespace

TEST_CASE("Quat: right-hand rule in the Z-up world (+90 degrees about +Z maps forward to left)")
{
    const Quat yaw90 = from_axis_angle({0, 0, 1}, half_pi);
    CHECK(near(rotate(yaw90, {1, 0, 0}), {0, 1, 0}));   // +X forward -> +Y left
    CHECK(near(rotate(yaw90, {0, 1, 0}), {-1, 0, 0}));  // left -> backward
    CHECK(near(rotate(yaw90, {0, 0, 1}), {0, 0, 1}));   // up is the axis: unchanged

    const Quat pitch90 = from_axis_angle({0, 1, 0}, half_pi);  // about +Y (left): positive pitch tips forward downward
    CHECK(near(rotate(pitch90, {1, 0, 0}), {0, 0, -1}));
    const Quat roll90 = from_axis_angle({1, 0, 0}, half_pi);
    CHECK(near(rotate(roll90, {0, 1, 0}), {0, 0, 1}));  // about +X: left -> up
}

TEST_CASE("Quat: composition applies the right operand first; conjugate inverts")
{
    const Quat yaw = from_axis_angle({0, 0, 1}, half_pi);
    const Quat pitch = from_axis_angle({0, 1, 0}, half_pi);
    // (yaw * pitch) v = yaw(pitch(v)): pitch first (x -> -z), then yaw leaves z alone.
    CHECK(near(rotate(yaw * pitch, {1, 0, 0}), {0, 0, -1}));
    // pitch(yaw(v)): yaw first (x -> y), then pitch about +Y leaves y alone.
    CHECK(near(rotate(pitch * yaw, {1, 0, 0}), {0, 1, 0}));

    const Float3 v{0.3f, -1.2f, 2.5f};
    CHECK(near(rotate(conjugate(yaw) * yaw, v), v));
    CHECK(near(rotate(conjugate(yaw), rotate(yaw, v)), v));
}

TEST_CASE("Quat: normalize is total (zero, NaN and unnormalized inputs never produce NaN)")
{
    CHECK(normalize(Quat{0, 0, 0, 0}) == Quat{});
    CHECK(normalize(Quat{std::numeric_limits<float>::quiet_NaN(), 0, 0, 1}) == Quat{});
    CHECK(normalize(Quat{std::numeric_limits<float>::infinity(), 0, 0, 1}) == Quat{});
    const Quat n = normalize({0, 0, 3, 4});
    CHECK(near(length(n), 1.0f));
    CHECK(near(n.z, 0.6f));
    CHECK(from_axis_angle({0, 0, 0}, 1.0f) == Quat{});  // zero axis
}

TEST_CASE("Quat: to_mat4 agrees with rotate()")
{
    const Quat q = normalize(from_axis_angle({1, 2, 3}, 1.1f) * from_axis_angle({0, 0, 1}, -0.4f));
    const Mat4 m = to_mat4(q);
    for (const Float3 v : {Float3{1, 0, 0}, Float3{0, 1, 0}, Float3{0, 0, 1}, Float3{0.5f, -2.0f, 3.0f}}) {
        CHECK(near(transform_direction(m, v), rotate(q, v), 1e-5f));
    }
    // A rotation matrix is orthonormal: M * M^T = I
    CHECK(near(m * transpose(m), Mat4::identity(), 1e-5f));
}

TEST_CASE("Quat: from_basis reproduces the basis it was built from")
{
    const Quat q = normalize(from_axis_angle({0.3f, -0.5f, 0.8f}, 2.2f));
    const Float3 f = rotate(q, {1, 0, 0});
    const Float3 l = rotate(q, {0, 1, 0});
    const Float3 u = rotate(q, {0, 0, 1});
    const Quat back = from_basis(f, l, u);
    CHECK(near(rotate(back, {1, 0, 0}), f, 1e-4f));
    CHECK(near(rotate(back, {0, 1, 0}), l, 1e-4f));
    CHECK(near(rotate(back, {0, 0, 1}), u, 1e-4f));
    // All four code paths of the matrix->quaternion conversion (trace and each dominant axis).
    for (const Quat probe : {from_axis_angle({1, 0, 0}, 3.0f), from_axis_angle({0, 1, 0}, 3.0f),
                             from_axis_angle({0, 0, 1}, 3.0f), from_axis_angle({1, 1, 1}, 0.5f)}) {
        const Quat r = from_basis(rotate(probe, {1, 0, 0}), rotate(probe, {0, 1, 0}), rotate(probe, {0, 0, 1}));
        CHECK(near(rotate(r, {1, 0, 0}), rotate(probe, {1, 0, 0}), 1e-4f));
        CHECK(near(rotate(r, {0, 1, 0}), rotate(probe, {0, 1, 0}), 1e-4f));
        CHECK(near(rotate(r, {0, 0, 1}), rotate(probe, {0, 0, 1}), 1e-4f));
    }
}

TEST_CASE("Quat: slerp interpolates along the shortest arc")
{
    const Quat a{};
    const Quat b = from_axis_angle({0, 0, 1}, half_pi);
    CHECK(near(rotate(slerp(a, b, 0.0f), {1, 0, 0}), {1, 0, 0}));
    CHECK(near(rotate(slerp(a, b, 1.0f), {1, 0, 0}), {0, 1, 0}));
    CHECK(near(rotate(slerp(a, b, 0.5f), {1, 0, 0}), {std::sqrt(0.5f), std::sqrt(0.5f), 0}, 1e-4f));
    // -b is the same rotation as b: the result must not swing the long way around.
    const Quat neg_b{-b.x, -b.y, -b.z, -b.w};
    CHECK(near(rotate(slerp(a, neg_b, 0.5f), {1, 0, 0}), {std::sqrt(0.5f), std::sqrt(0.5f), 0}, 1e-4f));
    // Nearly identical rotations take the lerp path without NaN.
    const Quat c = from_axis_angle({0, 0, 1}, 1e-5f);
    const Quat mid = slerp(a, c, 0.5f);
    CHECK(std::isfinite(mid.w));
    CHECK(near(length(mid), 1.0f));
}

TEST_CASE("Transform: matrix is T * R * S (scale, then rotate, then translate)")
{
    Transform t;
    t.position = {10, 20, 30};
    t.rotation = from_axis_angle({0, 0, 1}, half_pi);
    t.scale = {2, 3, 4};
    const Mat4 m = to_matrix(t);
    // Point (1,0,0): scale -> (2,0,0); yaw 90 -> (0,2,0); translate -> (10,22,30)
    CHECK(near(transform_point(m, {1, 0, 0}), {10, 22, 30}));
    // Point (0,1,0): scale -> (0,3,0); yaw 90 -> (-3,0,0); translate -> (7,20,30)
    CHECK(near(transform_point(m, {0, 1, 0}), {7, 20, 30}));
    CHECK(near(transform_point(m, {0, 0, 1}), {10, 20, 34}));
    // Directions ignore translation
    CHECK(near(transform_direction(m, {1, 0, 0}), {0, 2, 0}));
    // Identity transform
    CHECK(near(to_matrix(Transform{}), Mat4::identity()));
}

TEST_CASE("Transform: normal matrix keeps normals perpendicular under non-uniform scale")
{
    Transform t;
    t.rotation = from_axis_angle({0, 1, 1}, 0.7f);
    t.scale = {1.0f, 4.0f, 0.5f};
    const Mat4 m = to_matrix(t);
    const Mat4 n = normal_matrix(t);

    // A surface with tangents e1, e2 and normal e1 x e2: after transforming, normal . tangent must stay 0.
    const Float3 e1{1, 1, 0};
    const Float3 e2{0, 1, 1};
    const Float3 normal = cross(e1, e2);
    const Float3 t1 = transform_direction(m, e1);
    const Float3 t2 = transform_direction(m, e2);
    const Float3 tn = transform_direction(n, normal);
    CHECK(near(dot(tn, t1), 0.0f, 1e-4f));
    CHECK(near(dot(tn, t2), 0.0f, 1e-4f));

    // Using the model matrix instead would break perpendicularity (this is why the normal matrix exists).
    const Float3 wrong = transform_direction(m, normal);
    CHECK(std::fabs(dot(wrong, t1)) > 0.1f);

    // Uniform scale: the normal matrix is the rotation scaled by 1/s.
    Transform u;
    u.scale = {2, 2, 2};
    CHECK(near(transform_direction(normal_matrix(u), {0, 0, 1}), {0, 0, 0.5f}));
    // Zero scale never divides by zero.
    Transform z;
    z.scale = {0, 1, 1};
    CHECK(std::isfinite(normal_matrix(z).at(0, 0)));
}

TEST_CASE("Mat4: inverse of TRS matrices and of a projection-like matrix")
{
    Transform t;
    t.position = {3, -4, 5};
    t.rotation = from_axis_angle({1, 2, 3}, 0.9f);
    t.scale = {2, 0.5f, 3};
    const Mat4 m = to_matrix(t);
    const std::optional<Mat4> inv = inverse(m);
    REQUIRE(inv.has_value());
    CHECK(near(m * *inv, Mat4::identity(), 1e-4f));
    CHECK(near(*inv * m, Mat4::identity(), 1e-4f));
    CHECK(near(transform_point(*inv, transform_point(m, {1, 2, 3})), {1, 2, 3}, 1e-4f));

    const std::optional<Mat4> pinv = inverse(perspective_reverse_z(1.0f, 1.7f, 0.1f));
    REQUIRE(pinv.has_value());
    CHECK(near(perspective_reverse_z(1.0f, 1.7f, 0.1f) * *pinv, Mat4::identity(), 1e-4f));

    CHECK_FALSE(inverse(Mat4{}).has_value());  // all zeros is singular
    Mat4 flat = Mat4::identity();
    flat.at(1, 1) = 0.0f;
    CHECK_FALSE(inverse(flat).has_value());
    Mat4 nan = Mat4::identity();
    nan.at(0, 0) = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(inverse(nan).has_value());
}
