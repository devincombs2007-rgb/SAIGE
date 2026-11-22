#include "saige/gfx_assets/tangents.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace saige::gfx {

using render::cross;
using render::dot;
using render::Float3;
using render::length;
using render::normalize;

namespace {

Float3 position_of(const Primitive& p, uint32_t v)
{
    return {p.positions[v * 3], p.positions[v * 3 + 1], p.positions[v * 3 + 2]};
}

/// Angle at corner `a` of the triangle (a, b, c), in radians; 0 for a degenerate corner.
float corner_angle(Float3 a, Float3 b, Float3 c)
{
    const Float3 e1 = normalize(b - a);
    const Float3 e2 = normalize(c - a);
    return std::acos(std::clamp(dot(e1, e2), -1.0f, 1.0f));
}

/// Some unit vector perpendicular to `n`, deterministic: used when a vertex has no usable tangent.
Float3 any_perpendicular(Float3 n)
{
    const Float3 axis = std::fabs(n.x) < 0.9f ? Float3{1.0f, 0.0f, 0.0f} : Float3{0.0f, 1.0f, 0.0f};
    return normalize(axis - n * dot(axis, n));
}

} // namespace

TangentReport generate_tangents(Primitive& prim)
{
    TangentReport report;
    if (!prim.tangents.empty()) {
        report.result = TangentResult::AlreadyPresent;
        return report;
    }
    const std::size_t vertices = prim.positions.size() / 3;
    if (prim.uv0.empty()) {
        report.result = TangentResult::NoUvs;
        return report;
    }
    if (prim.positions.size() % 3 != 0 || prim.normals.size() != prim.positions.size() || prim.uv0.size() != vertices * 2 ||
        prim.indices.size() % 3 != 0 ||
        std::any_of(prim.indices.begin(), prim.indices.end(), [&](uint32_t i) { return i >= vertices; })) {
        report.result = TangentResult::Invalid;
        return report;
    }

    std::vector<Float3> tan(vertices);
    std::vector<Float3> bitan(vertices);
    for (std::size_t t = 0; t + 2 < prim.indices.size(); t += 3) {
        const uint32_t i0 = prim.indices[t];
        const uint32_t i1 = prim.indices[t + 1];
        const uint32_t i2 = prim.indices[t + 2];
        const Float3 p0 = position_of(prim, i0);
        const Float3 p1 = position_of(prim, i1);
        const Float3 p2 = position_of(prim, i2);
        const Float3 e1 = p1 - p0;
        const Float3 e2 = p2 - p0;
        // UVs with the origin at the top left; the bitangent follows +V *up*, which is -V in these coordinates.
        const float du1 = prim.uv0[i1 * 2] - prim.uv0[i0 * 2];
        const float dv1 = -(prim.uv0[i1 * 2 + 1] - prim.uv0[i0 * 2 + 1]);
        const float du2 = prim.uv0[i2 * 2] - prim.uv0[i0 * 2];
        const float dv2 = -(prim.uv0[i2 * 2 + 1] - prim.uv0[i0 * 2 + 1]);
        const float det = du1 * dv2 - du2 * dv1;
        if (!(std::fabs(det) > 1e-12f) || length(cross(e1, e2)) <= 1e-20f) {
            ++report.degenerate_triangles;
            continue;
        }
        const float inv = 1.0f / det;
        const Float3 tangent = (e1 * dv2 - e2 * dv1) * inv;
        const Float3 bitangent = (e2 * du1 - e1 * du2) * inv;
        const uint32_t corner[3] = {i0, i1, i2};
        const Float3 pos[3] = {p0, p1, p2};
        for (int c = 0; c < 3; ++c) {
            const float w = corner_angle(pos[c], pos[(c + 1) % 3], pos[(c + 2) % 3]);
            tan[corner[c]] = tan[corner[c]] + tangent * w;
            bitan[corner[c]] = bitan[corner[c]] + bitangent * w;
        }
    }

    prim.tangents.assign(vertices * 4, 0.0f);
    for (std::size_t v = 0; v < vertices; ++v) {
        const Float3 n = normalize(Float3{prim.normals[v * 3], prim.normals[v * 3 + 1], prim.normals[v * 3 + 2]});
        Float3 t = tan[v] - n * dot(n, tan[v]);
        float w = 1.0f;
        if (length(t) > 1e-12f && std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z)) {
            t = normalize(t);
            w = dot(cross(n, t), bitan[v]) < 0.0f ? -1.0f : 1.0f;
        } else {
            t = any_perpendicular(n);
            ++report.fallback_vertices;
        }
        prim.tangents[v * 4] = t.x;
        prim.tangents[v * 4 + 1] = t.y;
        prim.tangents[v * 4 + 2] = t.z;
        prim.tangents[v * 4 + 3] = w;
    }
    report.result = TangentResult::Generated;
    return report;
}

std::string to_string(TangentResult result)
{
    switch (result) {
    case TangentResult::Generated: return "generated";
    case TangentResult::AlreadyPresent: return "already present";
    case TangentResult::NoUvs: return "no uvs";
    case TangentResult::Invalid: return "invalid";
    }
    return "unknown";
}

} // namespace saige::gfx
