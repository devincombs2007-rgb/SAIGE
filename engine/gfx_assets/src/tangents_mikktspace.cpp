#include "saige/gfx_assets/tangents.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

extern "C" {
#include "mikktspace.h"
}

namespace saige::gfx {

namespace {

/// Some unit vector perpendicular to `n`, deterministic (the same rule as the Lengyel generator's fallback).
render::Float3 any_perpendicular(render::Float3 n)
{
    const render::Float3 axis = std::fabs(n.x) < 0.9f ? render::Float3{1.0f, 0.0f, 0.0f} : render::Float3{0.0f, 1.0f, 0.0f};
    return render::normalize(axis - n * render::dot(axis, n));
}

constexpr uint32_t kMaxTriangles = 50'000'000;
constexpr float kMergeCosine = 0.99985f;  // corner tangents within 1 degree (and the same sign) share a vertex

struct Context {
    const Primitive* primitive = nullptr;
    const uint32_t* indices = nullptr;  // null: sequential
    std::vector<float> tangent;         // xyz per corner
    std::vector<float> sign;            // per corner
};

uint32_t vertex_of(const Context& c, int face, int corner)
{
    const std::size_t k = static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(corner);
    return c.indices != nullptr ? c.indices[k] : static_cast<uint32_t>(k);
}

int get_num_faces(const SMikkTSpaceContext* mc)
{
    const Context& c = *static_cast<const Context*>(mc->m_pUserData);
    return static_cast<int>((c.indices != nullptr ? c.primitive->indices.size() : c.primitive->vertex_count()) / 3);
}
int get_num_vertices_of_face(const SMikkTSpaceContext*, const int) { return 3; }
void get_position(const SMikkTSpaceContext* mc, float out[], const int face, const int corner)
{
    const Context& c = *static_cast<const Context*>(mc->m_pUserData);
    const std::size_t v = vertex_of(c, face, corner);
    for (std::size_t i = 0; i < 3; ++i) out[i] = c.primitive->positions[v * 3 + i];
}
void get_normal(const SMikkTSpaceContext* mc, float out[], const int face, const int corner)
{
    const Context& c = *static_cast<const Context*>(mc->m_pUserData);
    const std::size_t v = vertex_of(c, face, corner);
    for (std::size_t i = 0; i < 3; ++i) out[i] = c.primitive->normals[v * 3 + i];
}
void get_tex_coord(const SMikkTSpaceContext* mc, float out[], const int face, const int corner)
{
    const Context& c = *static_cast<const Context*>(mc->m_pUserData);
    const std::size_t v = vertex_of(c, face, corner);
    out[0] = c.primitive->uv0[v * 2];
    out[1] = 1.0f - c.primitive->uv0[v * 2 + 1];  // glTF's V points down, MikkTSpace's up (as the Khronos sample viewer does)
}
void set_tspace_basic(const SMikkTSpaceContext* mc, const float tangent[], const float sign, const int face, const int corner)
{
    Context& c = *static_cast<Context*>(mc->m_pUserData);
    const std::size_t k = static_cast<std::size_t>(face) * 3 + static_cast<std::size_t>(corner);
    for (std::size_t i = 0; i < 3; ++i) c.tangent[k * 3 + i] = tangent[i];
    c.sign[k] = sign;
}

} // namespace

TangentReport generate_tangents_mikktspace(Primitive& p)
{
    TangentReport report;
    const std::size_t vertex_count = p.positions.size() / 3;
    const bool indexed = !p.indices.empty();
    const std::size_t corners = indexed ? p.indices.size() : vertex_count;
    if (p.positions.size() % 3 != 0 || p.normals.size() != p.positions.size() || corners % 3 != 0 || corners == 0 ||
        corners / 3 > kMaxTriangles || vertex_count > std::numeric_limits<uint32_t>::max() / 2 ||
        (!p.colors.empty() && p.colors.size() != vertex_count * 4)) {
        return report;  // Invalid
    }
    if (!p.tangents.empty()) {
        report.result = TangentResult::AlreadyPresent;
        return report;
    }
    if (p.uv0.empty()) {
        report.result = TangentResult::NoUvs;
        return report;
    }
    if (p.uv0.size() != vertex_count * 2) {
        return report;
    }
    for (const float f : p.positions) {
        if (!std::isfinite(f)) return report;
    }
    for (const float f : p.normals) {
        if (!std::isfinite(f)) return report;
    }
    for (const float f : p.uv0) {
        if (!std::isfinite(f)) return report;
    }
    for (const uint32_t i : p.indices) {
        if (i >= vertex_count) return report;
    }

    Context ctx;
    ctx.primitive = &p;
    ctx.indices = indexed ? p.indices.data() : nullptr;
    ctx.tangent.assign(corners * 3, 0.0f);
    ctx.sign.assign(corners, 1.0f);
    SMikkTSpaceInterface itf{};
    itf.m_getNumFaces = get_num_faces;
    itf.m_getNumVerticesOfFace = get_num_vertices_of_face;
    itf.m_getPosition = get_position;
    itf.m_getNormal = get_normal;
    itf.m_getTexCoord = get_tex_coord;
    itf.m_setTSpaceBasic = set_tspace_basic;
    SMikkTSpaceContext mc{&itf, &ctx};
    if (genTangSpaceDefault(&mc) == 0) {
        return report;  // Invalid
    }

    // Everything below builds locals; the primitive is not touched until the final, non-throwing publish.
    const auto normal_of = [&](uint32_t v) {
        return render::normalize(render::Float3{p.normals[std::size_t{v} * 3], p.normals[std::size_t{v} * 3 + 1], p.normals[std::size_t{v} * 3 + 2]});
    };
    // A corner's tangent as MikkTSpace returned it, made safe: finite, unit length and perpendicular to the vertex normal. A corner for
    // which that is impossible (a zero or non-finite result, or one parallel to the normal) gets a deterministic perpendicular frame.
    const auto sanitized = [&](std::size_t k, uint32_t v, float out[3], float& sign) {
        const render::Float3 n = normal_of(v);
        render::Float3 tg{ctx.tangent[k * 3], ctx.tangent[k * 3 + 1], ctx.tangent[k * 3 + 2]};
        sign = ctx.sign[k] >= 0.0f ? 1.0f : -1.0f;
        bool usable = std::isfinite(tg.x) && std::isfinite(tg.y) && std::isfinite(tg.z) && std::isfinite(ctx.sign[k]);
        if (usable) {
            tg = tg - n * render::dot(n, tg);  // orthogonalize against the normal (MikkTSpace already does; this guards the output)
            usable = render::length(tg) > 1e-3f && std::isfinite(render::length(tg));
        }
        if (usable) {
            tg = render::normalize(tg);
        } else {
            tg = any_perpendicular(n);
            sign = 1.0f;
            ++report.fallback_vertices;
        }
        out[0] = tg.x;
        out[1] = tg.y;
        out[2] = tg.z;
    };

    // Assign each corner a vertex: the original one for the first tangent seen there, a duplicate for each clearly different one.
    struct Assigned {
        float t[3];
        float s;
        uint32_t id;
    };
    std::vector<std::vector<Assigned>> by_vertex(vertex_count);
    std::vector<uint32_t> new_indices(corners);
    std::vector<float> tangents(vertex_count * 4, 0.0f);
    std::vector<uint32_t> duplicate_source;  // original vertex of each added vertex, in order
    for (std::size_t k = 0; k < corners; ++k) {
        const uint32_t v = indexed ? p.indices[k] : static_cast<uint32_t>(k);
        float t[3];
        float s;
        sanitized(k, v, t, s);
        std::vector<Assigned>& list = by_vertex[v];
        uint32_t id = std::numeric_limits<uint32_t>::max();
        for (const Assigned& a : list) {
            if (a.s == s && a.t[0] * t[0] + a.t[1] * t[1] + a.t[2] * t[2] >= kMergeCosine) {
                id = a.id;
                break;
            }
        }
        if (id == std::numeric_limits<uint32_t>::max()) {
            if (list.empty()) {
                id = v;
            } else {
                id = static_cast<uint32_t>(vertex_count + duplicate_source.size());
                duplicate_source.push_back(v);
                tangents.insert(tangents.end(), 4, 0.0f);
            }
            list.push_back({{t[0], t[1], t[2]}, s, id});
            tangents[static_cast<std::size_t>(id) * 4 + 0] = t[0];
            tangents[static_cast<std::size_t>(id) * 4 + 1] = t[1];
            tangents[static_cast<std::size_t>(id) * 4 + 2] = t[2];
            tangents[static_cast<std::size_t>(id) * 4 + 3] = s;
        }
        new_indices[k] = id;
    }
    // A vertex no triangle uses still gets a valid frame: a deterministic unit vector perpendicular to its own normal (a fixed +X would
    // not be perpendicular to every normal).
    for (std::size_t v = 0; v < vertex_count; ++v) {
        if (by_vertex[v].empty()) {
            const render::Float3 tg = any_perpendicular(normal_of(static_cast<uint32_t>(v)));
            tangents[v * 4 + 0] = tg.x;
            tangents[v * 4 + 1] = tg.y;
            tangents[v * 4 + 2] = tg.z;
            tangents[v * 4 + 3] = 1.0f;
        }
    }

    // Publish. Capacity for every appended element is reserved first: a reserve can throw, and does so before the primitive has changed;
    // the appends that follow then cannot allocate. The moves are non-throwing. So either the primitive comes out complete or it is
    // exactly as it was.
    const std::size_t total = vertex_count + duplicate_source.size();
    if (!duplicate_source.empty()) {
        p.positions.reserve(total * 3);
        p.normals.reserve(total * 3);
        p.uv0.reserve(total * 2);
        if (!p.colors.empty()) p.colors.reserve(total * 4);
        for (const uint32_t source : duplicate_source) {
            for (std::size_t i = 0; i < 3; ++i) p.positions.push_back(p.positions[static_cast<std::size_t>(source) * 3 + i]);
            for (std::size_t i = 0; i < 3; ++i) p.normals.push_back(p.normals[static_cast<std::size_t>(source) * 3 + i]);
            for (std::size_t i = 0; i < 2; ++i) p.uv0.push_back(p.uv0[static_cast<std::size_t>(source) * 2 + i]);
            if (!p.colors.empty()) {
                for (std::size_t i = 0; i < 4; ++i) p.colors.push_back(p.colors[static_cast<std::size_t>(source) * 4 + i]);
            }
        }
    }
    p.indices = std::move(new_indices);
    p.tangents = std::move(tangents);
    report.result = TangentResult::Generated;
    report.split_vertices = static_cast<uint32_t>(duplicate_source.size());
    for (std::size_t k = 0; k < corners; k += 3) {
        const float w0 = p.tangents[std::size_t{p.indices[k]} * 4 + 3];
        if (w0 != p.tangents[std::size_t{p.indices[k + 1]} * 4 + 3] || w0 != p.tangents[std::size_t{p.indices[k + 2]} * 4 + 3]) {
            ++report.mixed_handedness_triangles;
        }
    }
    return report;
}

} // namespace saige::gfx
