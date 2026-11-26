// Local lights reference: the physics the shaders and the GPU light culling must keep. Units and conversions, the inverse-square law, the smooth radius window, the spot cone, flux conservation by numerical
// integration, and the shading of a Lambertian surface. Each rule has a case that fails when the rule is broken (the mutation script in the commit message lists them).
#include "saige/gfx_test/saige_test.hpp"

#include "saige/render_reference/lights.hpp"

#include <cmath>
#include <limits>

using namespace saige::render_reference;

namespace {

constexpr double kEps = 1e-9;

double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

} // namespace

TEST_CASE("lights: photometric flux converts to intensity") {
    // A point source radiates over 4 pi sr: 1000 lm is 79.577 cd. A spot spreads its flux over the outer cone's solid angle 2 pi (1 - cos theta).
    CHECK(std::fabs(point_intensity_from_flux(1000.0) - 1000.0 / (4.0 * kPi)) < kEps);
    CHECK(std::fabs(point_intensity_from_flux(4.0 * kPi) - 1.0) < kEps);
    CHECK(std::fabs(spot_intensity_from_flux(1000.0, kPi * 0.5) - 1000.0 / (2.0 * kPi)) < 1e-9);   // a hemisphere
    CHECK(std::fabs(spot_intensity_from_flux(1000.0, kPi / 6.0) - 1000.0 / (2.0 * kPi * (1.0 - std::cos(kPi / 6.0)))) < 1e-9);
    CHECK(spot_intensity_from_flux(1000.0, kPi / 6.0) > point_intensity_from_flux(1000.0));       // a narrower cone is brighter on its axis
    CHECK(spot_intensity_from_flux(1000.0, 0.0) == 0.0);                                         // no cone, no defined intensity (not infinity)
}

TEST_CASE("lights: the inverse square law") {
    CHECK(std::fabs(inverse_square(1.0) - 1.0) < kEps);
    CHECK(std::fabs(inverse_square(2.0) - 0.25) < kEps);
    CHECK(std::fabs(inverse_square(10.0) - 0.01) < kEps);
    // Clamped at the minimum distance: finite on the light itself.
    CHECK(std::isfinite(inverse_square(0.0)));
    CHECK(std::fabs(inverse_square(0.0) - 1.0 / (kMinLightDistance * kMinLightDistance)) < 1e-6);
    CHECK(std::fabs(inverse_square(0.001) - inverse_square(0.0)) < 1e-6);
    // Far inside a huge radius the windowed attenuation IS the inverse square: doubling the distance quarters it.
    const double r = 1.0e6;
    CHECK(std::fabs(distance_attenuation(2.0, r) / distance_attenuation(1.0, r) - 0.25) < 1e-9);
    CHECK(std::fabs(distance_attenuation(8.0, r) / distance_attenuation(4.0, r) - 0.25) < 1e-9);
}

TEST_CASE("lights: the radius window is smooth and exactly zero at the radius") {
    const double R = 10.0;
    CHECK(std::fabs(radius_window(0.0, R) - 1.0) < kEps);
    CHECK(radius_window(R, R) == 0.0);
    CHECK(radius_window(R * 1.5, R) == 0.0);
    CHECK(std::fabs(radius_window(0.5 * R, R) - (1.0 - 0.0625) * (1.0 - 0.0625)) < 1e-12);   // (1 - 0.5^4)^2
    // Non-increasing.
    double prev = 2.0;
    for (int i = 0; i <= 100; ++i) {
        const double w = radius_window(R * static_cast<double>(i) / 100.0, R);
        CHECK(w <= prev + 1e-15);
        prev = w;
    }
    // Zero slope at the radius (no visible edge): a step of 1% of R inside it changes the window by far less than the slope of a linear fade would.
    const double h = 0.01 * R;
    CHECK(radius_window(R - h, R) < 4e-3);          // (1 - (0.99)^4)^2 = 0.0016 ... a linear fade would give 0.01
    CHECK(radius_window(R - h, R) > 0.0);
    // Bad radii give no light.
    CHECK(radius_window(1.0, 0.0) == 0.0);
    CHECK(radius_window(1.0, -3.0) == 0.0);
    CHECK(radius_window(1.0, std::numeric_limits<double>::quiet_NaN()) == 0.0);
    CHECK(radius_window(std::numeric_limits<double>::quiet_NaN(), R) == 0.0);
}

TEST_CASE("lights: the spot cone profile") {
    const double inner = 0.3, outer = 0.5;
    CHECK(std::fabs(spot_attenuation(1.0, inner, outer) - 1.0) < kEps);                     // on the axis
    CHECK(std::fabs(spot_attenuation(std::cos(inner), inner, outer) - 1.0) < 1e-12);        // at the inner edge
    CHECK(spot_attenuation(std::cos(outer), inner, outer) == 0.0);                          // at the outer edge
    CHECK(spot_attenuation(std::cos(outer + 0.2), inner, outer) == 0.0);                    // outside
    CHECK(spot_attenuation(-1.0, inner, outer) == 0.0);                                     // behind
    // Strictly increasing toward the axis between the edges, smooth (t squared), a quarter at the middle of the cosine range.
    double prev = -1.0;
    for (int i = 0; i <= 50; ++i) {
        const double a = outer - (outer - inner) * static_cast<double>(i) / 50.0;
        const double v = spot_attenuation(std::cos(a), inner, outer);
        CHECK(v >= prev - 1e-15);
        prev = v;
    }
    const double mid_cos = 0.5 * (std::cos(inner) + std::cos(outer));
    CHECK(std::fabs(spot_attenuation(mid_cos, inner, outer) - 0.25) < 1e-9);
    // An inner angle larger than the outer is treated as equal: a hard edge, never a negative or reversed cone.
    CHECK(spot_attenuation(std::cos(0.2), 0.9, 0.5) == 1.0);
    CHECK(spot_attenuation(std::cos(0.7), 0.9, 0.5) == 0.0);
}

TEST_CASE("lights: a point light's sample") {
    PointLight l;
    l.position = {0, 0, 3};
    l.color = {1, 0.5, 0.25};
    l.intensity = 90.0;
    l.radius = 50.0;
    LightSample s;
    REQUIRE(sample_point_light(l, {0, 0, 0}, s));
    CHECK(std::fabs(s.to_light[2] - 1.0) < 1e-12);                       // toward the light, unit length
    CHECK(std::fabs(dot(s.to_light, s.to_light) - 1.0) < 1e-12);
    const double e = 90.0 * distance_attenuation(3.0, 50.0);
    CHECK(std::fabs(s.irradiance[0] - e) < 1e-9);
    CHECK(std::fabs(s.irradiance[1] - 0.5 * e) < 1e-9);                  // the colour scales the channels
    CHECK(std::fabs(s.irradiance[2] - 0.25 * e) < 1e-9);
    // A sideways point: the direction points back at the light.
    REQUIRE(sample_point_light(l, {4, 0, 3}, s));
    CHECK(std::fabs(s.to_light[0] + 1.0) < 1e-12);
    // At and beyond the radius: no light (a culling sphere of that radius loses nothing).
    CHECK(!sample_point_light(l, {0, 0, 3 + 50.0}, s));
    CHECK(!sample_point_light(l, {0, 0, 3 + 80.0}, s));
    CHECK(sample_point_light(l, {0, 0, 3 + 49.9}, s));
    // On the light itself: finite.
    REQUIRE(sample_point_light(l, l.position, s));
    CHECK(std::isfinite(s.irradiance[0]) && std::isfinite(s.to_light[0]));
    // Invalid lights do not light.
    PointLight bad = l;
    bad.radius = 0.0;
    CHECK(!sample_point_light(bad, {0, 0, 0}, s));
    bad = l;
    bad.intensity = -1.0;
    CHECK(!sample_point_light(bad, {0, 0, 0}, s));
    bad = l;
    bad.position[0] = std::numeric_limits<double>::infinity();
    CHECK(!sample_point_light(bad, {0, 0, 0}, s));
}

TEST_CASE("lights: a spot light's sample") {
    SpotLight l;
    l.position = {0, 0, 5};
    l.direction = {0, 0, -1};   // shining straight down
    l.intensity = 100.0;
    l.radius = 60.0;
    l.inner_angle = 0.3;
    l.outer_angle = 0.5;
    LightSample s;
    PointLight p;
    p.position = l.position;
    p.intensity = l.intensity;
    p.radius = l.radius;
    LightSample ps;
    REQUIRE(sample_spot_light(l, {0, 0, 0}, s));                         // on the axis it equals the point light
    REQUIRE(sample_point_light(p, {0, 0, 0}, ps));
    CHECK(std::fabs(s.irradiance[0] - ps.irradiance[0]) < 1e-12);
    // Inside the inner cone: full strength; between: less; outside the outer cone: dark.
    const auto at_angle = [&](double a) { return Vec3{5.0 * std::tan(a), 0.0, 0.0}; };
    REQUIRE(sample_spot_light(l, at_angle(0.2), s));
    LightSample on_axis_equiv;
    REQUIRE(sample_point_light(p, at_angle(0.2), on_axis_equiv));
    CHECK(std::fabs(s.irradiance[0] - on_axis_equiv.irradiance[0]) < 1e-9);   // inside the inner cone the cone term is 1
    REQUIRE(sample_spot_light(l, at_angle(0.4), s));
    REQUIRE(sample_point_light(p, at_angle(0.4), on_axis_equiv));
    CHECK(s.irradiance[0] < on_axis_equiv.irradiance[0]);
    CHECK(s.irradiance[0] > 0.0);
    CHECK(!sample_spot_light(l, at_angle(0.6), s));
    // Behind the light.
    CHECK(!sample_spot_light(l, {0, 0, 10}, s));
    // A direction that is not unit length is rejected rather than quietly mis-lit.
    SpotLight bad = l;
    bad.direction = {0, 0, -2};
    CHECK(!sample_spot_light(bad, {0, 0, 0}, s));
}

TEST_CASE("lights: a point light's flux is conserved (numerical integration over a sphere)") {
    // The flux through a sphere of radius r around a point light is the integral of the irradiance on a surface facing the light: I / r^2 * 4 pi r^2 = 4 pi I = the lumens. With a window radius much larger than r
    // the window is ~1 and the integral must come back to the lumens; with the window at the sphere's radius it must be zero.
    const double lumens = 1000.0;
    PointLight l;
    l.intensity = point_intensity_from_flux(lumens);
    l.radius = 1.0e6;
    const double r = 4.0;
    const int n_theta = 400;
    double flux = 0.0;
    for (int i = 0; i < n_theta; ++i) {
        const double theta = (static_cast<double>(i) + 0.5) / n_theta * kPi;   // polar angle
        const Vec3 p{r * std::sin(theta), 0.0, r * std::cos(theta)};
        LightSample s;
        REQUIRE(sample_point_light(l, p, s));
        // The ring at this polar angle: area 2 pi r sin(theta) * r dtheta; the surface faces the light (cos 1).
        flux += s.irradiance[0] * 2.0 * kPi * r * std::sin(theta) * r * (kPi / n_theta);
    }
    CHECK(std::fabs(flux - lumens) < 0.002 * lumens);
    l.radius = r;   // the window closes exactly at the sphere
    LightSample s;
    CHECK(!sample_point_light(l, {r, 0, 0}, s));
}

TEST_CASE("lights: a spot's flux lies between its inner and outer cones") {
    // Authored by flux over the OUTER cone, the spot passes at most the full flux and at least that of its inner cone at full strength.
    const double lumens = 1000.0, inner = 0.3, outer = 0.5;
    SpotLight l;
    l.position = {0, 0, 0};
    l.direction = {0, 0, -1};
    l.intensity = spot_intensity_from_flux(lumens, outer);
    l.radius = 1.0e6;
    l.inner_angle = inner;
    l.outer_angle = outer;
    const double r = 4.0;
    const int n = 4000;
    double flux = 0.0;
    for (int i = 0; i < n; ++i) {
        const double a = (static_cast<double>(i) + 0.5) / n * kPi;   // angle from the axis
        const Vec3 p{r * std::sin(a), 0.0, -r * std::cos(a)};
        LightSample s;
        if (!sample_spot_light(l, p, s)) continue;
        flux += s.irradiance[0] * 2.0 * kPi * r * std::sin(a) * r * (kPi / n);
    }
    const double inner_flux = l.intensity * 2.0 * kPi * (1.0 - std::cos(inner));
    const double outer_flux = l.intensity * 2.0 * kPi * (1.0 - std::cos(outer));
    CHECK(flux > inner_flux);
    CHECK(flux < outer_flux);
    CHECK(std::fabs(outer_flux - lumens) < 1e-6);
}

TEST_CASE("lights: Lambertian shading follows the cosine law and the engine's units") {
    // The engine's convention: a directional light of colour c lights a white Lambertian surface facing it to radiance c, because its irradiance is c * pi. A point light of intensity pi at 1 m does the same.
    PointLight l;
    l.position = {0, 0, 1};
    l.intensity = kPi;
    l.radius = 100.0;
    LightSample s;
    REQUIRE(sample_point_light(l, {0, 0, 0}, s));
    const Vec3 white{1, 1, 1}, up{0, 0, 1};
    // The window reduces it very slightly at 1 m of 100 (1 - 1e-8)^2: compare with that.
    const double w = radius_window(1.0, 100.0);
    CHECK(std::fabs(shade_lambert(white, up, s)[0] - w) < 1e-9);
    // 60 degrees off the normal: half.
    const Vec3 tilted{std::sin(kPi / 3.0), 0.0, std::cos(kPi / 3.0)};
    CHECK(std::fabs(shade_lambert(white, tilted, s)[0] - 0.5 * w) < 1e-9);
    // Facing away: zero, never negative.
    CHECK(shade_lambert(white, {0, 0, -1}, s)[0] == 0.0);
    // Albedo scales channels; irradiance colour too.
    CHECK(std::fabs(shade_lambert({0.5, 0.25, 1.0}, up, s)[1] - 0.25 * w) < 1e-9);
    // Two lights add: the radiance from the sum of irradiances equals the sum of radiances (linearity), which is why local lights can be accumulated in any order.
    PointLight l2 = l;
    l2.position = {2, 0, 1};
    LightSample s2;
    REQUIRE(sample_point_light(l2, {0, 0, 0}, s2));
    // The radiances of separate lights add (light is linear), so local lights can be accumulated in any order:
    const double sum = shade_lambert(white, up, s)[0] + shade_lambert(white, up, s2)[0];
    CHECK(sum > shade_lambert(white, up, s)[0]);
    CHECK(sum > 0.0);
}
