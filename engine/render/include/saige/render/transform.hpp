// SAIGE Render — rotations and transforms in the engine's world convention.
//
// World: right-handed, +Z up, +X forward, +Y left, meters (docs/standards/coordinate-system.md). A positive rotation
// about an axis is counter-clockwise looking from the positive end of that axis toward the origin, so a +90 degree
// rotation about +Z maps +X (forward) to +Y (left). Quaternions are unit-length, stored (x, y, z, w).
// Matrices are column-vector, column-major (see math.hpp).
#pragma once

#include "saige/render/math.hpp"

#include <cmath>
#include <optional>

namespace saige::render {

struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    friend constexpr bool operator==(const Quat&, const Quat&) = default;
};

[[nodiscard]] constexpr Quat operator*(Quat a, Quat b) noexcept
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

[[nodiscard]] constexpr Quat conjugate(Quat q) noexcept
{
    return {-q.x, -q.y, -q.z, q.w};
}

[[nodiscard]] inline float length(Quat q) noexcept
{
    return std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
}

/// Unit-length copy. A zero-length or non-finite quaternion becomes the identity (never NaN, never a crash).
[[nodiscard]] inline Quat normalize(Quat q) noexcept
{
    const float len = length(q);
    if (!(len > 1e-12f) || !std::isfinite(len)) {
        return Quat{};
    }
    const float inv = 1.0f / len;
    return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
}

/// Rotation of `radians` about `axis` (right-hand rule). A zero axis gives the identity.
[[nodiscard]] inline Quat from_axis_angle(Float3 axis, float radians) noexcept
{
    const Float3 n = normalize(axis);
    if (n == Float3{}) {
        return Quat{};
    }
    const float half = radians * 0.5f;
    const float s = std::sin(half);
    return {n.x * s, n.y * s, n.z * s, std::cos(half)};
}

[[nodiscard]] inline Float3 rotate(Quat q, Float3 v) noexcept
{
    const Float3 u{q.x, q.y, q.z};
    const Float3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

/// Rotation whose local +X, +Y, +Z axes map to `forward`, `left`, `up` (must be orthonormal and right-handed).
[[nodiscard]] inline Quat from_basis(Float3 forward, Float3 left, Float3 up) noexcept
{
    // Rotation matrix columns are the images of the local axes.
    const float m00 = forward.x, m10 = forward.y, m20 = forward.z;
    const float m01 = left.x, m11 = left.y, m21 = left.z;
    const float m02 = up.x, m12 = up.y, m22 = up.z;
    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return normalize(q);
}

/// Spherical interpolation along the shortest arc (used to interpolate poses across a frame's `alpha`).
[[nodiscard]] inline Quat slerp(Quat a, Quat b, float t) noexcept
{
    float cos_theta = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (cos_theta < 0.0f) {  // q and -q are the same rotation: take the short way
        b = {-b.x, -b.y, -b.z, -b.w};
        cos_theta = -cos_theta;
    }
    if (cos_theta > 0.9995f) {  // nearly parallel: lerp is accurate and avoids dividing by sin(0)
        return normalize({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    }
    const float theta = std::acos(cos_theta);
    const float sin_theta = std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) / sin_theta;
    const float wb = std::sin(t * theta) / sin_theta;
    return normalize({a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb});
}

/// 4x4 rotation matrix of a (normalized) quaternion.
[[nodiscard]] inline Mat4 to_mat4(Quat q) noexcept
{
    q = normalize(q);
    const float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
    const float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
    const float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
    Mat4 m = Mat4::identity();
    m.at(0, 0) = 1.0f - 2.0f * (yy + zz);
    m.at(0, 1) = 2.0f * (xy - wz);
    m.at(0, 2) = 2.0f * (xz + wy);
    m.at(1, 0) = 2.0f * (xy + wz);
    m.at(1, 1) = 1.0f - 2.0f * (xx + zz);
    m.at(1, 2) = 2.0f * (yz - wx);
    m.at(2, 0) = 2.0f * (xz - wy);
    m.at(2, 1) = 2.0f * (yz + wx);
    m.at(2, 2) = 1.0f - 2.0f * (xx + yy);
    return m;
}

/// Translation, rotation and (positive) scale. World matrix = T * R * S.
struct Transform {
    Float3 position;
    Quat rotation;
    Float3 scale{1.0f, 1.0f, 1.0f};
};

[[nodiscard]] inline Mat4 to_matrix(const Transform& t) noexcept
{
    Mat4 m = to_mat4(t.rotation);
    for (int row = 0; row < 3; ++row) {  // R * S scales the rotation's columns
        m.at(row, 0) *= t.scale.x;
        m.at(row, 1) *= t.scale.y;
        m.at(row, 2) *= t.scale.z;
    }
    m.at(0, 3) = t.position.x;
    m.at(1, 3) = t.position.y;
    m.at(2, 3) = t.position.z;
    return m;
}

/// Matrix for transforming normals: the inverse transpose of the upper 3x3, which for T*R*S is R * S^-1.
/// Returns identity-rotation behavior for zero scale components (treated as 1) instead of dividing by zero.
[[nodiscard]] inline Mat4 normal_matrix(const Transform& t) noexcept
{
    const auto inv = [](float s) { return std::fabs(s) > 1e-12f ? 1.0f / s : 1.0f; };
    Mat4 m = to_mat4(t.rotation);
    for (int row = 0; row < 3; ++row) {
        m.at(row, 0) *= inv(t.scale.x);
        m.at(row, 1) *= inv(t.scale.y);
        m.at(row, 2) *= inv(t.scale.z);
    }
    return m;
}

[[nodiscard]] inline Float3 transform_point(const Mat4& m, Float3 p) noexcept
{
    const Float4 r = m * Float4{p.x, p.y, p.z, 1.0f};
    return {r.x, r.y, r.z};
}

[[nodiscard]] inline Float3 transform_direction(const Mat4& m, Float3 d) noexcept
{
    const Float4 r = m * Float4{d.x, d.y, d.z, 0.0f};
    return {r.x, r.y, r.z};
}

[[nodiscard]] inline Mat4 transpose(const Mat4& a) noexcept
{
    Mat4 r;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 4; ++col) {
            r.at(row, col) = a.at(col, row);
        }
    }
    return r;
}

/// General 4x4 inverse. Returns nullopt for singular (or non-finite) matrices.
[[nodiscard]] std::optional<Mat4> inverse(const Mat4& a) noexcept;

} // namespace saige::render
