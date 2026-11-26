// Surface wetness, puddles and the wet material model.
#include "saige/gfx_test/saige_test.hpp"

#include "saige/env/surface.hpp"

#include <stdexcept>
#include <cmath>

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

WeatherState weather(double rain_mm_h, double wind = 3.0, double sun_visibility = 0.3)
{
    WeatherState s;
    s.rain_mm_per_hour = rain_mm_h;
    s.wind_speed = wind;
    s.sun_visibility = sun_visibility;
    return s;
}
} // namespace

TEST_CASE("surface: wetness rises under rain toward 1, falls when it stops, and never leaves [0, 1]") {
    SurfaceProperties asphalt{0.6, 2.0, true};
    SurfaceWater water;
    double prev = 0.0;
    for (int i = 0; i < 600; ++i) {
        step_surface(water, asphalt, weather(10.0), 0.5, 1.0);
        REQUIRE(water.wetness >= prev - 1e-12);
        REQUIRE(water.wetness <= 1.0);
        prev = water.wetness;
    }
    CHECK(water.wetness > 0.95);
    for (int i = 0; i < 7200; ++i) {
        step_surface(water, asphalt, weather(0.0), 0.5, 1.0);
        REQUIRE(water.wetness <= prev + 1e-12);
        REQUIRE(water.wetness >= 0.0);
        prev = water.wetness;
    }
    CHECK(water.wetness < 0.2);
}

TEST_CASE("surface: wetness integration is exact, so slicing time differently gives the same film (checked away from equilibrium)") {
    SurfaceProperties s{0.5, 2.0, true};
    SurfaceWater a, b, c;
    step_surface(a, s, weather(3.0), 0.5, 10.0);
    for (int i = 0; i < 10; ++i) step_surface(b, s, weather(3.0), 0.5, 1.0);
    for (int i = 0; i < 1000; ++i) step_surface(c, s, weather(3.0), 0.5, 0.01);
    CHECK(a.wetness > 0.2);
    CHECK(a.wetness < 0.9);  // not yet at equilibrium, where any integrator would agree
    CHECK(std::abs(a.wetness - b.wetness) < 1e-12);
    CHECK(std::abs(a.wetness - c.wetness) < 1e-9);
}

TEST_CASE("surface: wind and sun dry a surface faster, and shade slower") {
    const SurfaceProperties s{0.5, 2.0, true};
    auto after = [&](double wind, double sun) {
        SurfaceWater w{1.0, 0.0};
        step_surface(w, s, weather(0.0, wind, 1.0), sun, 1800.0);
        return w.wetness;
    };
    CHECK(after(10.0, 1.0) < after(0.0, 1.0));
    CHECK(after(3.0, 1.0) < after(3.0, 0.0));
}

TEST_CASE("surface: puddles need saturation and rain beyond the drainage, outlive the damp film and respect allows_puddles") {
    const SurfaceProperties road{0.6, 2.0, true};
    SurfaceWater drizzle;
    for (int i = 0; i < 6 * 3600; ++i) step_surface(drizzle, road, weather(1.0), 0.0, 1.0);  // under the drainage rate
    CHECK(drizzle.puddle == 0.0);
    CHECK(drizzle.wetness > 0.9);

    SurfaceWater heavy;
    for (int i = 0; i < 3600; ++i) step_surface(heavy, road, weather(10.0), 0.0, 1.0);
    CHECK(heavy.puddle > 0.5);
    SurfaceWater after = heavy;
    for (int i = 0; i < 3600; ++i) step_surface(after, road, weather(0.0, 3.0, 0.3), 0.0, 1.0);
    CHECK(after.puddle > after.wetness);  // the street still shines after the film has gone
    CHECK(after.puddle < heavy.puddle);

    const SurfaceProperties roof{0.3, 2.0, false};
    SurfaceWater r;
    for (int i = 0; i < 3600; ++i) step_surface(r, roof, weather(20.0), 0.0, 1.0);
    CHECK(r.puddle == 0.0);
    CHECK(r.wetness > 0.9);
    CHECK(throws([&] { step_surface(r, roof, weather(1.0), 2.0, 1.0); }));
    CHECK(throws([&] { step_surface(r, roof, weather(1.0), 0.5, -1.0); }));
}

TEST_CASE("wet material: dry is bit-identical, wetting darkens porous surfaces and smooths the rest, and 0 and 1 are fixed points") {
    const SurfaceProperties porous{0.6, 2.0, true}, tight{0.0, 2.0, true};
    const std::array<double, 3> albedo{0.5, 0.3, 0.8};
    const WetMaterial dry = wet_surface(albedo, 0.7, porous, SurfaceWater{});
    CHECK(dry.albedo == albedo);
    CHECK(dry.roughness == 0.7);
    CHECK(dry.puddle_mask == 0.0);
    CHECK(dry.normal_flatten == 0.0);

    double prev_l = 1e9, prev_r = 1e9;
    for (double w = 0.0; w <= 1.0001; w += 0.1) {
        const WetMaterial m = wet_surface(albedo, 0.7, porous, SurfaceWater{std::min(w, 1.0), 0.0});
        REQUIRE(m.albedo[0] <= prev_l + 1e-12);
        REQUIRE(m.roughness <= prev_r + 1e-12);
        REQUIRE(m.roughness >= 0.0);
        prev_l = m.albedo[0];
        prev_r = m.roughness;
    }
    const WetMaterial wet = wet_surface(albedo, 0.7, porous, SurfaceWater{1.0, 0.0});
    CHECK(wet.albedo[0] < albedo[0]);
    CHECK(wet.albedo[1] < albedo[1]);
    const WetMaterial wet_tight = wet_surface(albedo, 0.7, tight, SurfaceWater{1.0, 0.0});
    CHECK(wet_tight.albedo == albedo);          // non-porous: no darkening
    CHECK(wet_tight.roughness < wet.roughness); // but it carries a film more readily
    const WetMaterial black = wet_surface({0.0, 0.0, 1.0}, 0.5, porous, SurfaceWater{1.0, 0.0});
    CHECK(black.albedo[0] == 0.0);
    CHECK(std::abs(black.albedo[2] - 1.0) < 1e-12);
    const WetMaterial smooth = wet_surface(albedo, 0.05, porous, SurfaceWater{1.0, 0.0});
    CHECK(smooth.roughness <= 0.05);  // a film only smooths
}

TEST_CASE("wet material: a full puddle is mirror smooth and flat, a small one is partial") {
    const SurfaceProperties s{0.6, 2.0, true};
    const WetMaterial full = wet_surface({0.4, 0.4, 0.4}, 0.8, s, SurfaceWater{1.0, 1.0});
    CHECK(full.puddle_mask == 1.0);
    CHECK(std::abs(full.roughness - 0.02) < 1e-12);
    CHECK(full.normal_flatten == 1.0);
    const WetMaterial part = wet_surface({0.4, 0.4, 0.4}, 0.8, s, SurfaceWater{1.0, 0.2});
    CHECK(part.puddle_mask > 0.0);
    CHECK(part.puddle_mask < 1.0);
    CHECK(throws([&] { (void)wet_surface({1.5, 0, 0}, 0.5, s, SurfaceWater{}); }));
    CHECK(throws([&] { (void)wet_surface({0.5, 0, 0}, 0.5, s, SurfaceWater{2.0, 0.0}); }));
}
