// Tests of generate_tangents_mikktspace (the pinned reference MikkTSpace behind a wrapper that maps its per-corner output to
// indexed vertices). Compiled only with SAIGE_GFX_MIKKTSPACE. The Lengyel-style generator is the importer's default; these tests
// are the bar MikkTSpace must clear before that changes.
#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/tangents.hpp"
#include "saige/render/math.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
#include <set>
#include <vector>

using namespace saige::gfx;
using saige::render::cross;
using saige::render::dot;
using saige::render::Float3;
using saige::render::length;

namespace {

constexpr float pi_f = 3.14159265358979f;

Float3 tangent_of(const Primitive& p, uint32_t v) { return {p.tangents[v * 4], p.tangents[v * 4 + 1], p.tangents[v * 4 + 2]}; }
Float3 normal_of(const Primitive& p, uint32_t v) { return {p.normals[v * 3], p.normals[v * 3 + 1], p.normals[v * 3 + 2]}; }
float sign_of(const Primitive& p, uint32_t v) { return p.tangents[v * 4 + 3]; }

/// A quad facing -X like the renderer's model tests: u grows toward -Y, so glTF's tangent is -Y, the bitangent (up in the image) is +Z.
Primitive quad_minus_x()
{
    Primitive p;
    p.positions = {0, +1, 2.5f, 0, -1, 2.5f, 0, +1, 0.5f, 0, -1, 0.5f};
    p.normals = {-1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0};
    p.uv0 = {0, 0, 1, 0, 0, 1, 1, 1};
    p.indices = {0, 2, 3, 0, 3, 1};
    return p;
}

/// mirror: 0 none; 1 lower half mirrored in UV and sharing the equator vertices (UVs fold across that band); 2 lower half
/// mirrored on its own duplicated equator vertices (how an exporter writes a mirrored island).
Primitive sphere(int lon, int lat, int mirror, float u_scale = 1.0f)
{
    Primitive p;
    for (int j = 0; j <= lat; ++j) {
        for (int i = 0; i <= lon; ++i) {  // the seam column is duplicated, as glTF requires
            const float th = pi_f * static_cast<float>(j) / static_cast<float>(lat), ph = 2.0f * pi_f * static_cast<float>(i) / static_cast<float>(lon);
            const float x = std::sin(th) * std::cos(ph), y = std::sin(th) * std::sin(ph), z = std::cos(th);
            p.positions.insert(p.positions.end(), {x, y, z});
            p.normals.insert(p.normals.end(), {x, y, z});
            float u = static_cast<float>(i) / static_cast<float>(lon) * u_scale;
            if ((mirror == 1 && j > lat / 2) || (mirror == 2 && j >= lat / 2)) u = u_scale - u;
            p.uv0.insert(p.uv0.end(), {u, static_cast<float>(j) / static_cast<float>(lat)});
        }
    }
    const uint32_t upper_equator = static_cast<uint32_t>((lat + 1) * (lon + 1));
    if (mirror == 2) {
        for (int i = 0; i <= lon; ++i) {
            const std::size_t s = static_cast<std::size_t>((lat / 2) * (lon + 1) + i);
            p.positions.insert(p.positions.end(), {p.positions[s * 3], p.positions[s * 3 + 1], p.positions[s * 3 + 2]});
            p.normals.insert(p.normals.end(), {p.normals[s * 3], p.normals[s * 3 + 1], p.normals[s * 3 + 2]});
            p.uv0.insert(p.uv0.end(), {static_cast<float>(i) / static_cast<float>(lon) * u_scale, static_cast<float>(lat / 2) / static_cast<float>(lat)});
        }
    }
    for (int j = 0; j < lat; ++j) {
        for (int i = 0; i < lon; ++i) {
            const uint32_t a = static_cast<uint32_t>(j * (lon + 1) + i), b = a + 1;
            uint32_t c = a + static_cast<uint32_t>(lon + 1), d = c + 1;
            if (mirror == 2 && j == lat / 2 - 1) {
                c = upper_equator + static_cast<uint32_t>(i);
                d = c + 1;
            }
            if (j != 0) p.indices.insert(p.indices.end(), {a, c, b});
            if (j != lat - 1) p.indices.insert(p.indices.end(), {b, c, d});
        }
    }
    return p;
}

double angle_degrees(Float3 a, Float3 b)
{
    const double d = static_cast<double>(dot(a, b)) / (static_cast<double>(length(a)) * static_cast<double>(length(b)));
    return std::acos(std::clamp(d, -1.0, 1.0)) * 180.0 / 3.14159265358979;
}

/// Every triangle corner: tangent unit length and perpendicular to the vertex normal; handedness equal across a triangle
/// (the glTF specification's rule); all indices in range; all arrays the same vertex count.
void check_well_formed(const Primitive& p)
{
    const uint32_t n = p.vertex_count();
    REQUIRE(p.tangents.size() == std::size_t{n} * 4);
    REQUIRE(p.normals.size() == std::size_t{n} * 3);
    REQUIRE(p.uv0.size() == std::size_t{n} * 2);
    size_t bad_length = 0, bad_perpendicular = 0, bad_handedness = 0, bad_index = 0;
    for (std::size_t k = 0; k < p.indices.size(); k += 3) {
        for (std::size_t c = 0; c < 3; ++c) {
            const uint32_t v = p.indices[k + c];
            if (v >= n) {
                ++bad_index;
                continue;
            }
            const Float3 t = tangent_of(p, v);
            bad_length += std::fabs(length(t) - 1.0f) > 1e-3f ? 1u : 0u;
            bad_perpendicular += std::fabs(dot(t, normal_of(p, v))) > 1e-3f ? 1u : 0u;
        }
        if (p.indices[k] < n && p.indices[k + 1] < n && p.indices[k + 2] < n) {
            bad_handedness += (sign_of(p, p.indices[k]) != sign_of(p, p.indices[k + 1]) || sign_of(p, p.indices[k]) != sign_of(p, p.indices[k + 2])) ? 1u : 0u;
        }
    }
    CHECK(bad_index == 0);
    CHECK(bad_length == 0);
    CHECK(bad_perpendicular == 0);
    CHECK(bad_handedness == 0);
}

} // namespace

TEST_CASE("MikkTSpace: a quad gets glTF's tangent frame (tangent along increasing u, bitangent up in the image)")
{
    Primitive p = quad_minus_x();
    const TangentReport r = generate_tangents_mikktspace(p);
    CHECK(r.result == TangentResult::Generated);
    CHECK(r.split_vertices == 0);
    REQUIRE(p.vertex_count() == 4);
    for (uint32_t v = 0; v < 4; ++v) {
        const Float3 t = tangent_of(p, v);
        CHECK(angle_degrees(t, {0, -1, 0}) < 0.01);
        CHECK(sign_of(p, v) == 1.0f);
        const Float3 bitangent = cross(normal_of(p, v), t) * sign_of(p, v);
        CHECK(angle_degrees(bitangent, {0, 0, 1}) < 0.01);  // up in the image
    }
    check_well_formed(p);
}

TEST_CASE("MikkTSpace: a mirrored UV mapping flips the handedness")
{
    Primitive p = quad_minus_x();
    for (std::size_t i = 0; i < p.uv0.size(); i += 2) p.uv0[i] = 1.0f - p.uv0[i];  // mirror u
    CHECK(generate_tangents_mikktspace(p).result == TangentResult::Generated);
    for (uint32_t v = 0; v < 4; ++v) {
        CHECK(angle_degrees(tangent_of(p, v), {0, +1, 0}) < 0.01);  // increasing u now points toward +Y
        CHECK(sign_of(p, v) == -1.0f);
    }
}

TEST_CASE("MikkTSpace: well-formed sphere meshes are orthonormal and agree with the Lengyel generator")
{
    for (const int mirror : {0, 2}) {
        for (const float u_scale : {1.0f, 4.0f}) {
            Primitive mikk = sphere(48, 24, mirror, u_scale);
            Primitive lengyel = mikk;
            const uint32_t before = mikk.vertex_count();
            REQUIRE(generate_tangents_mikktspace(mikk).result == TangentResult::Generated);
            REQUIRE(generate_tangents(lengyel).result == TangentResult::Generated);
            CHECK(mikk.vertex_count() == before);  // seams and mirrored islands were already split: nothing to duplicate
            check_well_formed(mikk);
            double worst = 0.0;
            size_t sign_differs = 0;
            for (const uint32_t v : mikk.indices) {
                // Same vertex ids: nothing was split, so compare the two directly.
                worst = std::max(worst, angle_degrees(tangent_of(mikk, v), tangent_of(lengyel, v)));
                sign_differs += sign_of(mikk, v) != sign_of(lengyel, v) ? 1u : 0u;
            }
            CHECK(worst < 1.0);        // measured 0.08 degrees on these meshes
            CHECK(sign_differs == 0);
        }
    }
}

TEST_CASE("MikkTSpace: a mirrored island that shares its seam vertices is split instead of blended")
{
    Primitive p = sphere(48, 24, 1);  // the equator vertices belong to mirrored and unmirrored triangles
    const uint32_t before = p.vertex_count();
    const TangentReport r = generate_tangents_mikktspace(p);
    CHECK(r.result == TangentResult::Generated);
    CHECK(r.split_vertices > 0);
    CHECK(p.vertex_count() == before + r.split_vertices);
    check_well_formed(p);  // equal handedness within every triangle after the split
    // Duplicates carry the original position, normal and uv exactly.
    for (uint32_t v = before; v < p.vertex_count(); ++v) {
        bool found = false;
        for (uint32_t o = 0; o < before && !found; ++o) {
            found = p.positions[o * 3] == p.positions[v * 3] && p.positions[o * 3 + 1] == p.positions[v * 3 + 1] &&
                    p.positions[o * 3 + 2] == p.positions[v * 3 + 2] && p.uv0[o * 2] == p.uv0[v * 2] && p.uv0[o * 2 + 1] == p.uv0[v * 2 + 1];
        }
        CHECK(found);
    }
}

TEST_CASE("MikkTSpace: unindexed triangle lists come out indexed, colors follow splits, results are deterministic")
{
    Primitive p = sphere(16, 8, 1);
    p.colors.assign(std::size_t{p.vertex_count()} * 4, 0.0f);
    for (uint32_t v = 0; v < p.vertex_count(); ++v) p.colors[std::size_t{v} * 4] = static_cast<float>(v);  // a per-vertex marker
    Primitive a = p, b = p;
    CHECK(generate_tangents_mikktspace(a).result == TangentResult::Generated);
    CHECK(generate_tangents_mikktspace(b).result == TangentResult::Generated);
    CHECK(a.tangents == b.tangents);
    CHECK(a.indices == b.indices);
    CHECK(a.positions == b.positions);
    CHECK(a.colors.size() == std::size_t{a.vertex_count()} * 4);
    // A duplicate's color is its source vertex's: the marker equals the index of a vertex with the same position.
    for (uint32_t v = p.vertex_count(); v < a.vertex_count(); ++v) {
        const uint32_t source = static_cast<uint32_t>(a.colors[std::size_t{v} * 4]);
        REQUIRE(source < p.vertex_count());
        CHECK(a.positions[std::size_t{source} * 3] == a.positions[std::size_t{v} * 3]);
    }

    Primitive flat;  // 2 triangles with no indices: 6 vertices
    flat.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0};
    flat.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
    flat.uv0 = {0, 1, 1, 1, 1, 0, 0, 1, 1, 0, 0, 0};
    CHECK(generate_tangents_mikktspace(flat).result == TangentResult::Generated);
    CHECK(flat.indices.size() == 6);
    check_well_formed(flat);
}

TEST_CASE("MikkTSpace: reports the same non-results as the Lengyel generator and never touches malformed input")
{
    Primitive p = quad_minus_x();
    p.uv0.clear();
    CHECK(generate_tangents_mikktspace(p).result == TangentResult::NoUvs);
    CHECK(p.tangents.empty());

    p = quad_minus_x();
    p.tangents.assign(16, 0.5f);
    const std::vector<float> existing = p.tangents;
    CHECK(generate_tangents_mikktspace(p).result == TangentResult::AlreadyPresent);
    CHECK(p.tangents == existing);

    p = quad_minus_x();
    p.indices.back() = 99;  // out of range
    const Primitive before = p;
    CHECK(generate_tangents_mikktspace(p).result == TangentResult::Invalid);
    CHECK(p.positions == before.positions);
    CHECK(p.indices == before.indices);
    CHECK(p.tangents.empty());

    p = quad_minus_x();
    p.normals.pop_back();
    CHECK(generate_tangents_mikktspace(p).result == TangentResult::Invalid);
    p = quad_minus_x();
    p.indices.pop_back();  // not a multiple of 3
    CHECK(generate_tangents_mikktspace(p).result == TangentResult::Invalid);
    p = quad_minus_x();
    p.uv0[3] = std::nanf("");
    CHECK(generate_tangents_mikktspace(p).result == TangentResult::Invalid);
    p = Primitive{};
    CHECK(generate_tangents_mikktspace(p).result == TangentResult::Invalid);
}

TEST_CASE("MikkTSpace: degenerate triangles do not poison their neighbours")
{
    Primitive p = quad_minus_x();
    // A zero-area triangle appended, sharing vertex 0 twice.
    p.indices.insert(p.indices.end(), {0, 0, 1});
    CHECK(generate_tangents_mikktspace(p).result == TangentResult::Generated);
    for (const float f : p.tangents) CHECK(std::isfinite(f));
    CHECK(angle_degrees(tangent_of(p, 1), {0, -1, 0}) < 0.5);
}

// ----------------------------------------------------------------------------------------------- review findings on #58

#include "alloc_fault.hpp"

namespace {

bool same_primitive(const Primitive& a, const Primitive& b)
{
    return a.positions == b.positions && a.normals == b.normals && a.uv0 == b.uv0 && a.colors == b.colors && a.indices == b.indices &&
           a.tangents == b.tangents;
}

} // namespace

TEST_CASE("MikkTSpace: an allocation failure at any point leaves the primitive exactly as it was")
{
    if (!saige::test::alloc_fault_available()) {
        std::printf("  allocation-failure injection is not available with this toolchain: sweep skipped\n");
        return;
    }
    Primitive base = sphere(16, 8, 1);  // shared mirrored equator: vertices are split, so every array grows
    base.colors.assign(std::size_t{base.vertex_count()} * 4, 0.5f);
    Primitive expected = base;
    const TangentReport good = generate_tangents_mikktspace(expected);
    REQUIRE(good.result == TangentResult::Generated);
    REQUIRE(good.split_vertices > 0);

    uint32_t failures = 0;
    bool succeeded = false;
    for (uint32_t n = 1; n < 100000 && !succeeded; ++n) {
        Primitive p = base;
        saige::test::fail_allocation_after(n);
        bool threw = false;
        try {
            (void)generate_tangents_mikktspace(p);
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        saige::test::fail_allocation_after(0);
        if (threw) {
            ++failures;
            CHECK(same_primitive(p, base));  // all arrays, indices and tangents unchanged: no half-grown vertex arrays
            CHECK(p.tangents.empty());
        } else {
            succeeded = true;
            CHECK(same_primitive(p, expected));  // enough allocations succeeded: the full, identical result
        }
    }
    CHECK(succeeded);
    CHECK(failures >= 10);  // not vacuous: the sweep failed many distinct allocation points, including the array growth
    std::printf("  allocation-failure sweep: %u failing allocation points, every one left the primitive unchanged\n", failures);
}

TEST_CASE("MikkTSpace: every output tangent is finite, unit length and perpendicular to its normal, including unused and degenerate vertices")
{
    // A vertex no triangle uses, with a normal along +X: a fixed +X tangent would be parallel to it.
    Primitive p = quad_minus_x();
    p.positions.insert(p.positions.end(), {5.0f, 5.0f, 5.0f});
    p.normals.insert(p.normals.end(), {1.0f, 0.0f, 0.0f});
    p.uv0.insert(p.uv0.end(), {0.3f, 0.3f});
    // A vertex whose normal is not a valid direction (zero length) used by a triangle, and a triangle with identical UVs on all corners.
    p.positions.insert(p.positions.end(), {0.0f, 3.0f, 1.0f, 0.0f, 4.0f, 1.0f, 0.0f, 3.0f, 2.0f});
    p.normals.insert(p.normals.end(), {0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f});
    p.uv0.insert(p.uv0.end(), {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f});
    p.indices.insert(p.indices.end(), {5, 6, 7});
    const TangentReport r = generate_tangents_mikktspace(p);
    REQUIRE(r.result == TangentResult::Generated);
    CHECK(r.fallback_vertices > 0);  // the degenerate triangle's corners
    for (uint32_t v = 0; v < p.vertex_count(); ++v) {
        const Float3 t = tangent_of(p, v);
        CHECK(std::isfinite(t.x));
        CHECK(std::isfinite(t.y));
        CHECK(std::isfinite(t.z));
        CHECK(std::fabs(length(t) - 1.0f) < 1e-3f);
        CHECK(std::fabs(dot(t, normal_of(p, v))) < 1e-3f);
        CHECK(std::fabs(std::fabs(sign_of(p, v)) - 1.0f) < 1e-6f);
    }
    // The unused vertex (index 4) is perpendicular to its +X normal, which the old fixed +X default was not.
    CHECK(std::fabs(dot(tangent_of(p, 4), Float3{1.0f, 0.0f, 0.0f})) < 1e-5f);
}
