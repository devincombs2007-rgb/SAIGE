// SAIGE Render — minimal renderer-internal math (D-07: Core owns Vec3 for positions; GPU layouts are packed and
// tested in the backend).
//
// Conventions (docs/standards/coordinate-system.md):
// - World: right-handed, +Z up, +X forward, +Y left, meters.
// - Matrices: column vectors (p' = M * p), column-major storage (m[column * 4 + row]); uploads to HLSL
//   `column_major float4x4` without transposition.
// - View space: right-handed, camera looks down -Z, +Y up.
// - Projection: reverse-Z with an infinite far plane (near -> depth 1, infinity -> depth 0), NDC +Y up.
#pragma once

#include <cmath>

namespace saige::render {

struct Float3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    friend constexpr bool operator==(const Float3&, const Float3&) = default;
};

struct Float4 {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
};

[[nodiscard]] constexpr Float3 operator+(Float3 a, Float3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
[[nodiscard]] constexpr Float3 operator-(Float3 a, Float3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
[[nodiscard]] constexpr Float3 operator*(Float3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
[[nodiscard]] constexpr float dot(Float3 a, Float3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
[[nodiscard]] constexpr Float3 cross(Float3 a, Float3 b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
[[nodiscard]] inline float length(Float3 a) noexcept { return std::sqrt(dot(a, a)); }
[[nodiscard]] inline Float3 normalize(Float3 a) noexcept
{
    const float len = length(a);
    return len > 0.0f ? a * (1.0f / len) : Float3{};
}

/// 4x4 matrix, column-major storage, column-vector convention.
struct Mat4 {
    float m[16] = {};

    [[nodiscard]] static constexpr Mat4 identity() noexcept
    {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    [[nodiscard]] constexpr float& at(int row, int column) noexcept { return m[column * 4 + row]; }
    [[nodiscard]] constexpr float at(int row, int column) const noexcept { return m[column * 4 + row]; }

    [[nodiscard]] friend constexpr Mat4 operator*(const Mat4& a, const Mat4& b) noexcept
    {
        Mat4 r;
        for (int c = 0; c < 4; ++c) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += a.at(row, k) * b.at(k, c);
                }
                r.at(row, c) = sum;
            }
        }
        return r;
    }

    [[nodiscard]] constexpr Float4 operator*(Float4 v) const noexcept
    {
        return {at(0, 0) * v.x + at(0, 1) * v.y + at(0, 2) * v.z + at(0, 3) * v.w,
                at(1, 0) * v.x + at(1, 1) * v.y + at(1, 2) * v.z + at(1, 3) * v.w,
                at(2, 0) * v.x + at(2, 1) * v.y + at(2, 2) * v.z + at(2, 3) * v.w,
                at(3, 0) * v.x + at(3, 1) * v.y + at(3, 2) * v.z + at(3, 3) * v.w};
    }
};

/// Right-handed view matrix: the camera at `eye` looks at `target`; `up` is the world up (+Z).
[[nodiscard]] inline Mat4 look_at(Float3 eye, Float3 target, Float3 up = {0.0f, 0.0f, 1.0f}) noexcept
{
    const Float3 f = normalize(target - eye);
    const Float3 s = normalize(cross(f, up));
    const Float3 u = cross(s, f);
    Mat4 v = Mat4::identity();
    v.at(0, 0) = s.x;
    v.at(0, 1) = s.y;
    v.at(0, 2) = s.z;
    v.at(1, 0) = u.x;
    v.at(1, 1) = u.y;
    v.at(1, 2) = u.z;
    v.at(2, 0) = -f.x;
    v.at(2, 1) = -f.y;
    v.at(2, 2) = -f.z;
    v.at(0, 3) = -dot(s, eye);
    v.at(1, 3) = -dot(u, eye);
    v.at(2, 3) = dot(f, eye);
    return v;
}

/// Reverse-Z, infinite far plane perspective. depth = near / -z_view, so z_view = -near -> 1 and z -> -inf -> 0.
[[nodiscard]] inline Mat4 perspective_reverse_z(float fov_y_radians, float aspect, float near_plane) noexcept
{
    const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
    Mat4 p;
    p.at(0, 0) = f / aspect;
    p.at(1, 1) = f;
    p.at(2, 3) = near_plane;
    p.at(3, 2) = -1.0f;
    return p;
}

inline constexpr float pi = 3.14159265358979323846f;

} // namespace saige::render
