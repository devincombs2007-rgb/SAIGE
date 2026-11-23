#include "saige/gfx_assets/tangents.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
    // An absent index buffer is a plain triangle list over the vertices (as everywhere else in the importer).
    const bool indexed = !prim.indices.empty();
    const std::size_t corner_count = indexed ? prim.indices.size() : vertices;
    if (prim.positions.size() % 3 != 0 || prim.normals.size() != prim.positions.size() || prim.uv0.size() != vertices * 2 ||
        corner_count % 3 != 0 || (!prim.colors.empty() && prim.colors.size() != vertices * 4) ||
        vertices > std::numeric_limits<uint32_t>::max() / 2 ||
        std::any_of(prim.indices.begin(), prim.indices.end(), [&](uint32_t i) { return i >= vertices; })) {
        report.result = TangentResult::Invalid;
        return report;
    }
    const auto corner_vertex = [&](std::size_t k) { return indexed ? prim.indices[k] : static_cast<uint32_t>(k); };

    // Accumulate per vertex *and per UV orientation*: a vertex shared by triangles whose UV mapping is mirrored relative to each
    // other has no single tangent frame (averaging would give a mixed handedness inside one face), so such a vertex is split below.
    // Group 0: UV area positive; group 1: negative.
    std::vector<Float3> tan[2] = {std::vector<Float3>(vertices), std::vector<Float3>(vertices)};
    std::vector<Float3> bitan[2] = {std::vector<Float3>(vertices), std::vector<Float3>(vertices)};
    std::vector<uint8_t> used[2] = {std::vector<uint8_t>(vertices, 0), std::vector<uint8_t>(vertices, 0)};
    std::vector<int8_t> group_of_triangle(corner_count / 3, -1);  // -1: degenerate, contributes nothing
    for (std::size_t t = 0; t < corner_count; t += 3) {
        const uint32_t i0 = corner_vertex(t), i1 = corner_vertex(t + 1), i2 = corner_vertex(t + 2);
        const Float3 p0 = position_of(prim, i0), p1 = position_of(prim, i1), p2 = position_of(prim, i2);
        const Float3 e1 = p1 - p0, e2 = p2 - p0;
        // UVs with the origin at the top left; the bitangent follows +V *up*, which is -V in these coordinates.
        const float du1 = prim.uv0[i1 * 2] - prim.uv0[i0 * 2];
        const float dv1 = -(prim.uv0[i1 * 2 + 1] - prim.uv0[i0 * 2 + 1]);
        const float du2 = prim.uv0[i2 * 2] - prim.uv0[i0 * 2];
        const float dv2 = -(prim.uv0[i2 * 2 + 1] - prim.uv0[i0 * 2 + 1]);
        const float det = du1 * dv2 - du2 * dv1;
        if (!(std::fabs(det) > 1e-12f) || length(cross(e1, e2)) <= 1e-20f || !std::isfinite(det)) {
            ++report.degenerate_triangles;
            continue;
        }
        const int g = det > 0.0f ? 0 : 1;
        group_of_triangle[t / 3] = static_cast<int8_t>(g);
        const float inv = 1.0f / det;
        const Float3 tangent = (e1 * dv2 - e2 * dv1) * inv;
        const Float3 bitangent = (e2 * du1 - e1 * du2) * inv;
        const uint32_t corner[3] = {i0, i1, i2};
        const Float3 pos[3] = {p0, p1, p2};
        for (int c = 0; c < 3; ++c) {
            const float w = corner_angle(pos[c], pos[(c + 1) % 3], pos[(c + 2) % 3]);
            tan[g][corner[c]] = tan[g][corner[c]] + tangent * w;
            bitan[g][corner[c]] = bitan[g][corner[c]] + bitangent * w;
            used[g][corner[c]] = 1;
        }
    }

    // Vertices used by both orientations get a second copy for group 1, in ascending vertex order (deterministic).
    std::vector<uint32_t> duplicate_of;  // original vertex of each added vertex
    std::vector<uint32_t> group1_id(vertices, 0);
    for (std::size_t v = 0; v < vertices; ++v) {
        group1_id[v] = static_cast<uint32_t>(v);
        if (used[0][v] != 0 && used[1][v] != 0) {
            group1_id[v] = static_cast<uint32_t>(vertices + duplicate_of.size());
            duplicate_of.push_back(static_cast<uint32_t>(v));
        }
    }
    const std::size_t total = vertices + duplicate_of.size();

    // Build the complete result in locals; the primitive is changed only at the very end, with noexcept moves, so an exception while
    // allocating leaves it exactly as it was.
    std::vector<float> tangents(total * 4, 0.0f);
    const auto write_tangent = [&](std::size_t out, std::size_t v, int g) {
        const Float3 n = normalize(Float3{prim.normals[v * 3], prim.normals[v * 3 + 1], prim.normals[v * 3 + 2]});
        Float3 t = tan[g][v] - n * dot(n, tan[g][v]);
        float w = 1.0f;
        if (length(t) > 1e-12f && std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z)) {
            t = normalize(t);
            w = dot(cross(n, t), bitan[g][v]) < 0.0f ? -1.0f : 1.0f;
        } else {
            t = any_perpendicular(n);
            ++report.fallback_vertices;
        }
        tangents[out * 4] = t.x;
        tangents[out * 4 + 1] = t.y;
        tangents[out * 4 + 2] = t.z;
        tangents[out * 4 + 3] = w;
    };
    for (std::size_t v = 0; v < vertices; ++v) {
        write_tangent(v, v, used[0][v] != 0 ? 0 : (used[1][v] != 0 ? 1 : 0));  // an unused vertex gets the fallback frame
    }
    for (std::size_t d = 0; d < duplicate_of.size(); ++d) {
        write_tangent(vertices + d, duplicate_of[d], 1);
    }

    if (duplicate_of.empty()) {
        prim.tangents = std::move(tangents);
        report.result = TangentResult::Generated;
    } else {
        std::vector<float> positions = prim.positions, normals = prim.normals, uv0 = prim.uv0, colors = prim.colors;
        positions.reserve(total * 3);
        normals.reserve(total * 3);
        uv0.reserve(total * 2);
        if (!colors.empty()) colors.reserve(total * 4);
        for (const uint32_t source : duplicate_of) {
            for (std::size_t c = 0; c < 3; ++c) positions.push_back(prim.positions[std::size_t{source} * 3 + c]);
            for (std::size_t c = 0; c < 3; ++c) normals.push_back(prim.normals[std::size_t{source} * 3 + c]);
            for (std::size_t c = 0; c < 2; ++c) uv0.push_back(prim.uv0[std::size_t{source} * 2 + c]);
            if (!prim.colors.empty()) {
                for (std::size_t c = 0; c < 4; ++c) colors.push_back(prim.colors[std::size_t{source} * 4 + c]);
            }
        }
        std::vector<uint32_t> indices = prim.indices;  // splitting only happens for indexed input (an unindexed vertex has one triangle)
        for (std::size_t t = 0; t < indices.size(); t += 3) {
            if (group_of_triangle[t / 3] == 1) {
                for (std::size_t c = 0; c < 3; ++c) indices[t + c] = group1_id[indices[t + c]];
            }
        }
        prim.positions = std::move(positions);
        prim.normals = std::move(normals);
        prim.uv0 = std::move(uv0);
        prim.colors = std::move(colors);
        prim.indices = std::move(indices);
        prim.tangents = std::move(tangents);
        report.split_vertices = static_cast<uint32_t>(duplicate_of.size());
        report.result = TangentResult::Generated;
    }

    // Triangles whose corners still disagree on handedness (normals that contradict the winding): reported, not hidden.
    const auto handedness = [&](std::size_t k) {
        const std::size_t v = prim.indices.empty() ? k : prim.indices[k];
        return prim.tangents[v * 4 + 3];
    };
    for (std::size_t t = 0; t < corner_count; t += 3) {
        if (handedness(t) != handedness(t + 1) || handedness(t) != handedness(t + 2)) {
            ++report.mixed_handedness_triangles;
        }
    }
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
