#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/tangents.hpp"
#include "saige/render/math.hpp"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace saige::gfx;
using saige::render::cross;
using saige::render::dot;
using saige::render::Float3;
using saige::render::length;

namespace {

constexpr float pi_f = 3.14159265358979f;

bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

Float3 tangent_of(const Primitive& p, uint32_t v) { return {p.tangents[v * 4], p.tangents[v * 4 + 1], p.tangents[v * 4 + 2]}; }
Float3 normal_of(const Primitive& p, uint32_t v) { return {p.normals[v * 3], p.normals[v * 3 + 1], p.normals[v * 3 + 2]}; }

/// A unit quad in the XY plane facing +Z. UVs use the top-left origin: world +Y maps to *decreasing* V, so the image's "up" is
/// world +Y, as the viewer sees it. `u_scale`/`v_scale` of -1 mirror the mapping.
Primitive quad(float u_scale = 1.0f, float v_scale = 1.0f)
{
    Primitive p;
    p.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
    p.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    const float u[4] = {0, 1, 1, 0};
    const float v[4] = {1, 1, 0, 0};  // y = 0 -> v = 1 (bottom of the image), y = 1 -> v = 0 (top)
    for (int i = 0; i < 4; ++i) {
        const float uu = u_scale >= 0 ? u[i] : 1.0f - u[i];
        const float vv = v_scale >= 0 ? v[i] : 1.0f - v[i];
        p.uv0.push_back(uu);
        p.uv0.push_back(vv);
    }
    p.indices = {0, 1, 2, 0, 2, 3};
    return p;
}

float handedness(const Primitive& p, uint32_t v) { return p.tangents[v * 4 + 3]; }

} // namespace

TEST_CASE("Tangents: a quad gets +X tangents, unit length, perpendicular to the normal, handedness +1")
{
    Primitive p = quad();
    const TangentReport r = generate_tangents(p);
    CHECK(r.result == TangentResult::Generated);
    CHECK(r.degenerate_triangles == 0);
    CHECK(r.fallback_vertices == 0);
    REQUIRE(p.tangents.size() == 16);
    for (uint32_t v = 0; v < 4; ++v) {
        const Float3 t = tangent_of(p, v);
        CHECK(near(t.x, 1.0f));
        CHECK(near(t.y, 0.0f));
        CHECK(near(t.z, 0.0f));
        CHECK(near(length(t), 1.0f));
        CHECK(near(dot(t, normal_of(p, v)), 0.0f));
        CHECK(near(handedness(p, v), 1.0f));
        // The glTF bitangent is cross(normal, tangent) * w and points up the image (world +Y here).
        const Float3 b = cross(normal_of(p, v), t) * handedness(p, v);
        CHECK(near(b.y, 1.0f));
    }
}

TEST_CASE("Tangents: a mirrored mapping flips the handedness, not the frame's orthogonality")
{
    Primitive flip_v = quad(1.0f, -1.0f);  // V runs the other way: the image appears upside down
    CHECK(generate_tangents(flip_v).result == TangentResult::Generated);
    for (uint32_t v = 0; v < 4; ++v) {
        CHECK(near(tangent_of(flip_v, v).x, 1.0f));  // U still increases along +X
        CHECK(near(handedness(flip_v, v), -1.0f));   // so bitangent = cross(N, T) * w now points down (-Y)
    }
    Primitive flip_u = quad(-1.0f, 1.0f);  // U runs along -X
    CHECK(generate_tangents(flip_u).result == TangentResult::Generated);
    for (uint32_t v = 0; v < 4; ++v) {
        CHECK(near(tangent_of(flip_u, v).x, -1.0f));
        CHECK(near(handedness(flip_u, v), -1.0f));
    }
}

TEST_CASE("Tangents: a rotated mapping rotates the tangent with it")
{
    // U increases along world +Y and the image's up is world -X.
    Primitive p = quad();
    for (uint32_t v = 0; v < 4; ++v) {
        const float x = p.positions[v * 3];
        const float y = p.positions[v * 3 + 1];
        p.uv0[v * 2] = y;           // u = y
        p.uv0[v * 2 + 1] = x;       // v (down) = x, so up = -x
    }
    CHECK(generate_tangents(p).result == TangentResult::Generated);
    for (uint32_t v = 0; v < 4; ++v) {
        const Float3 t = tangent_of(p, v);
        CHECK(near(t.y, 1.0f));
        const Float3 b = cross(normal_of(p, v), t) * handedness(p, v);
        CHECK(near(b.x, -1.0f));
    }
}

TEST_CASE("Tangents: a smooth shared-vertex sphere follows the longitude direction")
{
    // A UV sphere: u around the equator (increasing with longitude), v from the top pole (0) to the bottom pole (1).
    constexpr int slices = 32;
    constexpr int stacks = 16;
    Primitive p;
    for (int i = 0; i <= stacks; ++i) {
        const float v = static_cast<float>(i) / stacks;
        const float phi = v * pi_f;  // 0 at the north pole (+Z)
        for (int j = 0; j <= slices; ++j) {
            const float u = static_cast<float>(j) / slices;
            const float theta = u * 2.0f * pi_f;
            const Float3 n{std::sin(phi) * std::cos(theta), std::sin(phi) * std::sin(theta), std::cos(phi)};
            p.positions.insert(p.positions.end(), {n.x, n.y, n.z});
            p.normals.insert(p.normals.end(), {n.x, n.y, n.z});
            p.uv0.insert(p.uv0.end(), {u, v});
        }
    }
    const auto at = [&](int i, int j) { return static_cast<uint32_t>(i * (slices + 1) + j); };
    for (int i = 0; i < stacks; ++i) {
        for (int j = 0; j < slices; ++j) {
            // Counter-clockwise seen from outside.
            p.indices.insert(p.indices.end(), {at(i, j), at(i + 1, j), at(i, j + 1)});
            p.indices.insert(p.indices.end(), {at(i, j + 1), at(i + 1, j), at(i + 1, j + 1)});
        }
    }
    const TangentReport r = generate_tangents(p);
    CHECK(r.result == TangentResult::Generated);
    int checked = 0;
    for (int i = 2; i < stacks - 1; ++i) {  // away from the poles, where the mapping degenerates
        for (int j = 1; j < slices; ++j) {
            const uint32_t v = at(i, j);
            const float theta = static_cast<float>(j) / slices * 2.0f * pi_f;
            const Float3 expected{-std::sin(theta), std::cos(theta), 0.0f};  // d(position)/du, normalized
            const Float3 t = tangent_of(p, v);
            CHECK(dot(t, expected) > 0.995f);
            CHECK(near(dot(t, normal_of(p, v)), 0.0f, 1e-3f));
            // The bitangent points toward increasing up-in-the-image V, which is toward the north pole (+Z side).
            const Float3 b = cross(normal_of(p, v), t) * handedness(p, v);
            CHECK(b.z > 0.0f);
            ++checked;
        }
    }
    CHECK(checked > 100);
}

TEST_CASE("Tangents: degenerate input falls back to a finite, unit frame and is reported")
{
    Primitive p = quad();
    for (float& uv : p.uv0) uv = 0.25f;  // every UV identical: zero UV area everywhere
    const TangentReport r = generate_tangents(p);
    CHECK(r.result == TangentResult::Generated);
    CHECK(r.degenerate_triangles == 2);
    CHECK(r.fallback_vertices == 4);
    for (uint32_t v = 0; v < 4; ++v) {
        const Float3 t = tangent_of(p, v);
        CHECK(std::isfinite(t.x) && std::isfinite(t.y) && std::isfinite(t.z));
        CHECK(near(length(t), 1.0f));
        CHECK(near(dot(t, normal_of(p, v)), 0.0f));
        CHECK(near(handedness(p, v), 1.0f));
    }

    Primitive collapsed = quad();
    collapsed.positions = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};  // zero-area triangles
    CHECK(generate_tangents(collapsed).result == TangentResult::Generated);
    for (float t : collapsed.tangents) CHECK(std::isfinite(t));
}

TEST_CASE("Tangents: existing tangents are kept, and missing or inconsistent inputs store nothing")
{
    Primitive given = quad();
    given.tangents = {0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    const std::vector<float> before = given.tangents;
    CHECK(generate_tangents(given).result == TangentResult::AlreadyPresent);
    CHECK(given.tangents == before);

    Primitive no_uv = quad();
    no_uv.uv0.clear();
    CHECK(generate_tangents(no_uv).result == TangentResult::NoUvs);
    CHECK(no_uv.tangents.empty());

    Primitive bad_index = quad();
    bad_index.indices[2] = 99;
    CHECK(generate_tangents(bad_index).result == TangentResult::Invalid);
    CHECK(bad_index.tangents.empty());

    Primitive bad_sizes = quad();
    bad_sizes.normals.pop_back();
    CHECK(generate_tangents(bad_sizes).result == TangentResult::Invalid);

    Primitive ragged = quad();
    ragged.indices.pop_back();  // not a whole number of triangles
    CHECK(generate_tangents(ragged).result == TangentResult::Invalid);
}

TEST_CASE("Tangents: generation is deterministic and leaves every other array untouched")
{
    Primitive a = quad();
    Primitive b = quad();
    const Primitive original = quad();
    generate_tangents(a);
    generate_tangents(b);
    CHECK(a.tangents == b.tangents);
    CHECK(a.positions == original.positions);
    CHECK(a.normals == original.normals);
    CHECK(a.uv0 == original.uv0);
    CHECK(a.indices == original.indices);
    CHECK(to_string(TangentResult::Generated) == "generated");
}

// ----------------------------------------------------------------------------------------------- review findings on #46

namespace {

/// Two quads in the XY plane (facing +Z) sharing the edge x = 1: the left maps u = x, the right is mirrored (u = 2 - x), so the UVs are
/// continuous across the shared vertices but the mapping's orientation flips. V runs so that world +Y is image-up (v = 1 - y).
Primitive mirrored_strip(bool with_colors = false)
{
    Primitive p;
    const float xs[3] = {0.0f, 1.0f, 2.0f};
    for (int row = 0; row < 2; ++row) {
        for (int col = 0; col < 3; ++col) {
            p.positions.insert(p.positions.end(), {xs[col], static_cast<float>(row), 0.0f});
            p.normals.insert(p.normals.end(), {0.0f, 0.0f, 1.0f});
            const float u = xs[col] <= 1.0f ? xs[col] : 2.0f - xs[col];
            p.uv0.insert(p.uv0.end(), {u, 1.0f - static_cast<float>(row)});
            if (with_colors) p.colors.insert(p.colors.end(), {static_cast<float>(row * 3 + col), 0.0f, 0.0f, 1.0f});
        }
    }
    // Vertices: row 0 = 0,1,2 (y = 0), row 1 = 3,4,5 (y = 1). Counter-clockwise seen from +Z.
    p.indices = {0, 1, 4, 0, 4, 3,   // left quad
                 1, 2, 5, 1, 5, 4};  // right quad
    return p;
}

} // namespace

TEST_CASE("Tangents: an unindexed triangle list gets real tangents, not the fallback frame")
{
    // The same two triangles as the quad helper's, written out as a plain list of six vertices (no index buffer).
    Primitive indexed = quad();
    Primitive flat;
    for (const uint32_t i : indexed.indices) {
        flat.positions.insert(flat.positions.end(), {indexed.positions[i * 3], indexed.positions[i * 3 + 1], indexed.positions[i * 3 + 2]});
        flat.normals.insert(flat.normals.end(), {indexed.normals[i * 3], indexed.normals[i * 3 + 1], indexed.normals[i * 3 + 2]});
        flat.uv0.insert(flat.uv0.end(), {indexed.uv0[i * 2], indexed.uv0[i * 2 + 1]});
    }
    REQUIRE(flat.indices.empty());
    const TangentReport report = generate_tangents(flat);
    CHECK(report.result == TangentResult::Generated);
    CHECK(report.fallback_vertices == 0);  // before the fix every vertex took the arbitrary fallback frame
    CHECK(report.split_vertices == 0);
    CHECK(flat.indices.empty());           // still unindexed
    REQUIRE(flat.tangents.size() == 6 * 4);
    CHECK(generate_tangents(indexed).result == TangentResult::Generated);
    for (uint32_t v = 0; v < 6; ++v) {
        const Float3 t = tangent_of(flat, v);
        CHECK(near(t.x, 1.0f) );  // the quad helper maps +X to increasing u
        CHECK(near(t.y, 0.0f));
        CHECK(near(flat.tangents[v * 4 + 3], 1.0f));
    }
    // A list that is not a whole number of triangles is invalid, and nothing changes.
    Primitive ragged = flat;
    ragged.tangents.clear();
    ragged.positions.resize(5 * 3);
    ragged.normals.resize(5 * 3);
    ragged.uv0.resize(5 * 2);
    CHECK(generate_tangents(ragged).result == TangentResult::Invalid);
    CHECK(ragged.tangents.empty());
}

TEST_CASE("Tangents: vertices shared by mirrored UV islands are split, so no face has mixed handedness")
{
    Primitive p = mirrored_strip(/*with_colors=*/true);
    const Primitive before = p;
    const TangentReport report = generate_tangents(p);
    CHECK(report.result == TangentResult::Generated);
    CHECK(report.split_vertices == 2);                // the two vertices on the shared edge
    CHECK(report.mixed_handedness_triangles == 0);
    REQUIRE(p.vertex_count() == before.vertex_count() + 2);
    CHECK(p.tangents.size() == std::size_t{p.vertex_count()} * 4);
    CHECK(p.colors.size() == std::size_t{p.vertex_count()} * 4);
    // Left quad: increasing u is +X, right-handed. Right quad (mirrored): increasing u is -X, left-handed. Each triangle's three corners agree.
    for (std::size_t t = 0; t < p.indices.size(); t += 3) {
        const bool left = t < 6;
        for (int c = 0; c < 3; ++c) {
            const uint32_t v = p.indices[t + static_cast<std::size_t>(c)];
            const Float3 tg = tangent_of(p, v);
            CHECK(near(tg.x, left ? 1.0f : -1.0f));
            CHECK(near(tg.y, 0.0f));
            CHECK(near(p.tangents[v * 4 + 3], left ? 1.0f : -1.0f));
            CHECK(near(dot(tg, normal_of(p, v)), 0.0f));
        }
    }
    // The left quad keeps its original vertices; the added copies carry position, normal, uv and color of their source exactly.
    for (std::size_t k = 0; k < 6; ++k) CHECK(p.indices[k] == before.indices[k]);
    for (uint32_t v = before.vertex_count(); v < p.vertex_count(); ++v) {
        bool found = false;
        for (uint32_t o = 0; o < before.vertex_count() && !found; ++o) {
            found = p.positions[v * 3] == before.positions[o * 3] && p.positions[v * 3 + 1] == before.positions[o * 3 + 1] &&
                    p.uv0[v * 2] == before.uv0[o * 2] && p.uv0[v * 2 + 1] == before.uv0[o * 2 + 1] && p.colors[v * 4] == before.colors[o * 4];
        }
        CHECK(found);
    }
    // Deterministic.
    Primitive again = mirrored_strip(true);
    generate_tangents(again);
    CHECK(again.tangents == p.tangents);
    CHECK(again.indices == p.indices);
}

TEST_CASE("Tangents: mirrored islands that the file already split are left alone, and a mirrored whole mesh is not split")
{
    Primitive whole = quad(-1.0f, 1.0f);  // every triangle mirrored the same way
    const TangentReport r = generate_tangents(whole);
    CHECK(r.split_vertices == 0);
    CHECK(whole.vertex_count() == 4);
    CHECK(near(whole.tangents[3], -1.0f));
}

TEST_CASE("Tangents: inconsistent colors are rejected before anything changes")
{
    Primitive p = mirrored_strip(true);
    p.colors.pop_back();
    const Primitive before = p;
    CHECK(generate_tangents(p).result == TangentResult::Invalid);
    CHECK(p.positions == before.positions);
    CHECK(p.indices == before.indices);
    CHECK(p.tangents.empty());
}
