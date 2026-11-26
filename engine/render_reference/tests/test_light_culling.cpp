// Clustered light culling reference: the grid (every point of the frustum lies in the box of the cluster its coordinates name), the sphere-box test, and the list's contract (ascending, truncated, overflow counted).
#include "saige/gfx_test/saige_test.hpp"

#include "saige/render_reference/light_culling.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>

using namespace saige::render_reference;

namespace {

template <typename Fn>
bool throws_invalid(Fn&& fn)
{
    try {
        fn();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

struct Rng {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    double next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return static_cast<double>(s >> 11) / 9007199254740992.0; }
    double range(double a, double b) { return a + (b - a) * next(); }
};

} // namespace

TEST_CASE("light culling: slice depths are exponential, start at near and end at far")
{
    ClusterGrid g;
    CHECK(std::fabs(slice_depth(g, 0) - g.near_plane) < 1e-12);
    CHECK(std::fabs(slice_depth(g, g.nz) - g.far_plane) < 1e-9);
    for (int k = 0; k < g.nz; ++k) CHECK(slice_depth(g, k + 1) > slice_depth(g, k));
    const double r0 = slice_depth(g, 1) / slice_depth(g, 0), r1 = slice_depth(g, 11) / slice_depth(g, 10);
    CHECK(std::fabs(r0 - r1) < 1e-9);   // a constant ratio: exponential
    ClusterGrid bad = g;
    bad.far_plane = bad.near_plane;
    CHECK(!bad.valid());
    bad = g;
    bad.nz = 0;
    CHECK(!bad.valid());
}

TEST_CASE("light culling: every point of the frustum lies in the box of the cluster its screen position and depth name")
{
    ClusterGrid g;
    const std::vector<Aabb> boxes = build_cluster_boxes(g);
    CHECK(boxes.size() == static_cast<size_t>(g.count()));
    Rng rng;
    int checked = 0;
    for (int n = 0; n < 20000; ++n) {
        const double z = std::exp(rng.range(std::log(g.near_plane), std::log(g.far_plane) - 1e-9));
        const double ndc_x = rng.range(-0.999999, 0.999999), ndc_y = rng.range(-0.999999, 0.999999);
        const std::array<double, 3> p{ndc_x * g.tan_half_fov_y * g.aspect * z, ndc_y * g.tan_half_fov_y * z, z};
        const int ix = static_cast<int>((ndc_x + 1.0) * 0.5 * g.nx), iy = static_cast<int>((ndc_y + 1.0) * 0.5 * g.ny);
        int iz = 0;
        while (iz + 1 < g.nz && z >= slice_depth(g, iz + 1)) ++iz;
        const Aabb& b = boxes[static_cast<size_t>((iz * g.ny + iy) * g.nx + ix)];
        bool inside = true;
        for (size_t a = 0; a < 3; ++a) inside = inside && p[a] >= b.lo[a] - 1e-9 && p[a] <= b.hi[a] + 1e-9;
        CHECK(inside);
        ++checked;
    }
    CHECK(checked == 20000);
}

TEST_CASE("light culling: a sphere meets a box when the squared distance from its centre to the box is at most radius squared")
{
    const Aabb box{{0, 0, 0}, {2, 2, 2}};
    CHECK(sphere_meets_aabb({1, 1, 1}, 0.1, box));            // inside
    CHECK(sphere_meets_aabb({3, 1, 1}, 1.0, box));            // touching a face
    CHECK(!sphere_meets_aabb({3, 1, 1}, 0.99, box));
    CHECK(sphere_meets_aabb({3, 3, 3}, std::sqrt(3.0) + 1e-12, box));   // touching a corner (a hair over: sqrt(3)^2 rounds below 3)
    CHECK(!sphere_meets_aabb({3, 3, 3}, 1.7, box));
    CHECK(sphere_meets_aabb({-5, 1, 1}, 5.0, box));
    CHECK(!sphere_meets_aabb({-5, 1, 1}, 4.9, box));
    CHECK(sphere_meets_aabb({1, 1, 1}, 0.0, box));            // a zero radius inside the box
    CHECK(!sphere_meets_aabb({5, 5, 5}, 0.0, box));
}

TEST_CASE("light culling: lists are ascending, truncated at the maximum with the overflow counted, and no light that meets a cluster is lost beyond the truncation")
{
    ClusterGrid g;
    g.nx = 4; g.ny = 2; g.nz = 3;
    const std::vector<Aabb> boxes = build_cluster_boxes(g);
    Rng rng;
    std::vector<CullLight> lights;
    for (int i = 0; i < 40; ++i) lights.push_back({{rng.range(-30, 30), rng.range(-20, 20), rng.range(-5, 150)}, rng.range(0.5, 40.0)});
    const CullResult full = cull_lights(g, boxes, lights, 64);
    const CullResult cut = cull_lights(g, boxes, lights, 3);
    int truncated_clusters = 0;
    for (size_t c = 0; c < boxes.size(); ++c) {
        uint32_t met = 0;
        for (size_t i = 0; i < lights.size(); ++i) met += sphere_meets_aabb(lights[i].view_position, lights[i].radius, boxes[c]) ? 1u : 0u;
        CHECK(full.counts[c] == met && full.overflow[c] == 0);
        for (uint32_t k = 1; k < full.counts[c]; ++k) CHECK(full.indices[c * 64 + k - 1] < full.indices[c * 64 + k]);
        CHECK(cut.counts[c] == std::min<uint32_t>(met, 3) && cut.counts[c] + cut.overflow[c] == met);
        for (uint32_t k = 0; k < cut.counts[c]; ++k) CHECK(cut.indices[c * 3 + k] == full.indices[c * 64 + k]);   // the truncated list is the first entries of the full one
        for (uint32_t k = cut.counts[c]; k < 3; ++k) CHECK(cut.indices[c * 3 + k] == 0xFFFFFFFFu);
        truncated_clusters += cut.overflow[c] > 0 ? 1 : 0;
    }
    CHECK(truncated_clusters > 0);   // the truncation was exercised
    const CullResult none = cull_lights(g, boxes, {}, 4);
    for (size_t c = 0; c < boxes.size(); ++c) CHECK(none.counts[c] == 0 && none.overflow[c] == 0);
    CHECK(throws_invalid([&] { (void)cull_lights(g, boxes, lights, 0); }));
}
