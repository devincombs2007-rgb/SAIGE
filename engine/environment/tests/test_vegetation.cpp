// Vegetation: wind bend (length-preserving, bounded), leaf flutter, and the stable grass scatter with LOD thinning that never pops.
#include "saige/gfx_test/saige_test.hpp"

#include "saige/env/vegetation.hpp"

#include <stdexcept>
#include <algorithm>
#include <cmath>
#include <map>

using namespace saige::env;

namespace {
template <class F>
bool throws(F&& f)
{
    try {
        f();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}
double norm(const std::array<double, 3>& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
} // namespace

TEST_CASE("vegetation: the sway angle is zero in still air, grows with wind, saturates and shrinks with stiffness") {
    PlantWind tree;
    CHECK(sway_angle(tree, 0.0, 12.3, 0.4) == 0.0);
    double prev = 0.0;
    for (double v = 0.0; v <= 40.0; v += 1.0) {
        const double a = sway_angle(tree, v, 3.0, 1.0);
        REQUIRE(a >= prev - 1e-12);
        REQUIRE(a <= tree.max_angle * 1.25 + 1e-12);
        prev = a;
    }
    PlantWind stiff = tree;
    stiff.stiffness = 2.0;
    CHECK(sway_angle(stiff, 10.0, 3.0, 1.0) < sway_angle(tree, 10.0, 3.0, 1.0));
    CHECK(natural_frequency_hz(0.5) > natural_frequency_hz(6.0));
    CHECK(std::abs(natural_frequency_hz(6.0) - 0.5 / std::sqrt(6.0)) < 1e-12);
    // The rocking is bounded: over a period the angle varies by the 25% rock.
    double lo = 1e9, hi = -1e9;
    for (double t = 0.0; t < 20.0; t += 0.05) {
        const double a = sway_angle(tree, 12.0, t, 0.0);
        lo = std::min(lo, a);
        hi = std::max(hi, a);
    }
    CHECK(hi > lo * 1.3);
    CHECK(throws([&] { (void)sway_angle(tree, -1.0, 0.0, 0.0); }));
    CHECK(throws([&] { PlantWind bad; bad.height_m = 0.0; (void)sway_angle(bad, 1.0, 0.0, 0.0); }));
}

TEST_CASE("vegetation: bending keeps every vertex's distance from the base, leaves the base alone and tips the top downwind") {
    const double H = 6.0;
    for (const double angle : {0.0, 0.1, 0.35, 0.5}) {
        for (const std::array<double, 3> p : {std::array<double, 3>{0.0, 0.0, 6.0}, {0.5, -0.3, 3.0}, {-1.2, 0.8, 5.0}, {0.2, 0.2, 0.0}, {0.0, 1.0, 9.0}}) {
            const auto q = bend_position(p, H, {1.0, 0.0}, angle);
            REQUIRE(std::abs(norm(q) - norm(p)) < 1e-12);
        }
    }
    CHECK(bend_position({0.3, 0.2, 0.0}, H, {1.0, 0.0}, 0.4) == (std::array<double, 3>{0.3, 0.2, 0.0}));
    CHECK(bend_position({0.0, 0.0, 3.0}, H, {1.0, 0.0}, 0.0) == (std::array<double, 3>{0.0, 0.0, 3.0}));
    const auto tip = bend_position({0.0, 0.0, 6.0}, H, {1.0, 0.0}, 0.3);
    CHECK(tip[0] > 0.5);   // leans toward +x
    CHECK(std::abs(tip[1]) < 1e-12);
    CHECK(tip[2] < 6.0);
    const auto north = bend_position({0.0, 0.0, 6.0}, H, {0.0, 5.0}, 0.3);  // the direction need not be normalised
    CHECK(north[1] > 0.5);
    CHECK(std::abs(north[0]) < 1e-12);
    const auto mid = bend_position({0.0, 0.0, 3.0}, H, {1.0, 0.0}, 0.3);
    CHECK(mid[0] > 0.0);
    CHECK(mid[0] < tip[0]);  // the tip leans furthest
    CHECK(bend_position({0.0, 0.0, 6.0}, H, {0.0, 0.0}, 0.3) == (std::array<double, 3>{0.0, 0.0, 6.0}));
    CHECK(throws([&] { (void)bend_position({0, 0, 1}, 0.0, {1, 0}, 0.1); }));
}

TEST_CASE("vegetation: leaf flutter is bounded, zero without wind or mask, and along the normal") {
    const std::array<double, 3> n{0.0, 0.6, 0.8};
    double worst = 0.0;
    for (double t = 0.0; t < 10.0; t += 0.013) {
        const auto f = leaf_flutter(n, 1.0, 20.0, t, 0.7);
        worst = std::max(worst, norm(f));
        REQUIRE(norm(f) <= 0.03 + 1e-12);
        // Parallel to the normal: the cross product's length is zero.
        const double cx = f[1] * n[2] - f[2] * n[1], cy = f[2] * n[0] - f[0] * n[2], cz = f[0] * n[1] - f[1] * n[0];
        REQUIRE(std::sqrt(cx * cx + cy * cy + cz * cz) < 1e-12);
    }
    CHECK(worst > 0.029);
    CHECK(norm(leaf_flutter(n, 1.0, 0.0, 1.234, 0.1)) == 0.0);
    CHECK(norm(leaf_flutter(n, 0.0, 15.0, 1.234, 0.1)) == 0.0);
    CHECK(norm(leaf_flutter({0, 0, 0}, 1.0, 15.0, 1.234, 0.1)) == 0.0);
    CHECK(throws([&] { (void)leaf_flutter(n, 1.5, 1.0, 0.0, 0.0); }));
}

TEST_CASE("grass: scattering is deterministic, independent of the requested region and keeps its minimum spacing") {
    GrassConfig cfg;
    cfg.seed = 11;
    const auto big = scatter_grass(cfg, -5.0, -5.0, 5.0, 5.0);
    const auto again = scatter_grass(cfg, -5.0, -5.0, 5.0, 5.0);
    REQUIRE(big.size() == again.size());
    for (std::size_t i = 0; i < big.size(); ++i) {
        REQUIRE(big[i].id == again[i].id);
        REQUIRE(big[i].x == again[i].x);
    }
    // A sub-region returns exactly the instances of the big region that fall inside it, with the same ids and positions.
    const auto sub = scatter_grass(cfg, -1.0, 0.0, 2.0, 3.0);
    std::map<std::uint64_t, GrassInstance> by_id;
    for (const auto& g : big) by_id[g.id] = g;
    std::size_t expected = 0;
    for (const auto& g : big)
        if (g.x >= -1.0 && g.x < 2.0 && g.y >= 0.0 && g.y < 3.0) ++expected;
    CHECK(sub.size() == expected);
    for (const auto& g : sub) {
        REQUIRE(by_id.count(g.id) == 1);
        CHECK(by_id[g.id].x == g.x);
        CHECK(by_id[g.id].yaw == g.yaw);
    }
    // Spacing: no two clusters closer than cell * (1 - jitter).
    const double min_gap = cfg.cell * (1.0 - cfg.jitter) - 1e-9;
    double closest = 1e9;
    for (std::size_t i = 0; i < big.size(); ++i)
        for (std::size_t j = i + 1; j < big.size(); ++j) closest = std::min(closest, std::hypot(big[i].x - big[j].x, big[i].y - big[j].y));
    CHECK(closest >= min_gap);
    for (const auto& g : big) {
        REQUIRE(g.scale >= 0.7);
        REQUIRE(g.scale <= 1.3);
    }
    // Different seeds grow different grass.
    GrassConfig other = cfg;
    other.seed = 12;
    const auto o = scatter_grass(other, -5.0, -5.0, 5.0, 5.0);
    CHECK(o.front().x != big.front().x);
}

TEST_CASE("grass: the count follows the cell size and the density mask") {
    GrassConfig cfg;
    cfg.seed = 3;
    const auto full = scatter_grass(cfg, 0.0, 0.0, 20.0, 20.0);
    const double expected = 400.0 / (cfg.cell * cfg.cell);
    CHECK(std::abs(static_cast<double>(full.size()) - expected) < 0.03 * expected);
    const auto half = scatter_grass(cfg, 0.0, 0.0, 20.0, 20.0, [](double, double) { return 0.5; });
    CHECK(std::abs(static_cast<double>(half.size()) / static_cast<double>(full.size()) - 0.5) < 0.05);
    CHECK(scatter_grass(cfg, 0.0, 0.0, 20.0, 20.0, [](double, double) { return 0.0; }).empty());
    // A mask that is 1 on the left half only.
    const auto left = scatter_grass(cfg, 0.0, 0.0, 20.0, 20.0, [](double x, double) { return x < 10.0 ? 1.0 : 0.0; });
    for (const auto& g : left) REQUIRE(g.x < 10.0);
    CHECK(left.size() > full.size() / 3);
    CHECK(throws([&] { (void)scatter_grass(cfg, 1.0, 0.0, 1.0, 2.0); }));
    GrassConfig bad = cfg;
    bad.far = bad.near;
    CHECK(throws([&] { (void)scatter_grass(bad, 0.0, 0.0, 1.0, 1.0); }));
}

TEST_CASE("grass: distance thinning is monotonic and nested (nothing pops in as the camera backs away), and the widening holds the coverage") {
    GrassConfig cfg;
    cfg.seed = 5;
    const auto grass = scatter_grass(cfg, 0.0, 0.0, 30.0, 30.0);
    REQUIRE(grass.size() > 5000);
    CHECK(lod_fraction(cfg, 0.0) == 1.0);
    CHECK(lod_fraction(cfg, cfg.near) == 1.0);
    CHECK(std::abs(lod_fraction(cfg, cfg.far) - cfg.min_fraction) < 1e-12);
    CHECK(std::abs(lod_fraction(cfg, 10.0 * cfg.far) - cfg.min_fraction) < 1e-12);
    double prev = 1.0, prev_scale = 1.0;
    for (double d = 0.0; d <= 120.0; d += 1.0) {
        const double f = lod_fraction(cfg, d);
        REQUIRE(f <= prev + 1e-12);
        REQUIRE(lod_width_scale(cfg, d) >= prev_scale - 1e-12);
        prev = f;
        prev_scale = lod_width_scale(cfg, d);
    }
    // Nested: kept at a farther distance implies kept at every nearer one.
    for (const auto& g : grass)
        for (double d = 0.0; d < 110.0; d += 11.0)
            if (lod_keep(cfg, g, d + 11.0)) REQUIRE(lod_keep(cfg, g, d));
    // The kept share matches the fraction, and width x share holds the covered area.
    for (const double d : {40.0, 60.0, 80.0}) {
        std::size_t kept = 0;
        for (const auto& g : grass) kept += lod_keep(cfg, g, d) ? 1u : 0u;
        const double share = static_cast<double>(kept) / static_cast<double>(grass.size());
        CHECK(std::abs(share - lod_fraction(cfg, d)) < 0.03);
        const double scale = lod_width_scale(cfg, d);
        CHECK(std::abs(share * scale * scale - 1.0) < 0.1);
    }
    CHECK(throws([&] { (void)lod_fraction(cfg, -1.0); }));
}
