#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/atmosphere.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

using namespace saige::render;

namespace {

constexpr double deg = 3.14159265358979323846 / 180.0;

Float3 dir_from(double elevation_deg, double azimuth_deg)
{
    const double e = elevation_deg * deg, a = azimuth_deg * deg;
    return {static_cast<float>(std::cos(e) * std::cos(a)), static_cast<float>(std::cos(e) * std::sin(a)), static_cast<float>(std::sin(e))};
}

bool finite3(Float3 c) { return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z); }

bool throws_invalid(const AtmosphereSettings& a)
{
    try {
        validate_atmosphere(a);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

} // namespace

TEST_CASE("Atmosphere: settings are validated")
{
    const AtmosphereSettings good;
    CHECK(!throws_invalid(good));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const auto bad = [&](auto&& mutate) {
        AtmosphereSettings a;
        mutate(a);
        return throws_invalid(a);
    };
    CHECK(bad([&](AtmosphereSettings& a) { a.planet_radius_km = nan; }));
    CHECK(bad([](AtmosphereSettings& a) { a.planet_radius_km = 0.0f; }));
    CHECK(bad([](AtmosphereSettings& a) { a.atmosphere_height_km = -1.0f; }));
    CHECK(bad([](AtmosphereSettings& a) { a.rayleigh_scale_height_km = 0.0f; }));
    CHECK(bad([](AtmosphereSettings& a) { a.mie_scale_height_km = 0.0f; }));
    CHECK(bad([](AtmosphereSettings& a) { a.rayleigh_coefficient.y = 0.0f; }));
    CHECK(bad([](AtmosphereSettings& a) { a.mie_coefficient = -0.1f; }));
    CHECK(bad([](AtmosphereSettings& a) { a.mie_g = 1.0f; }));
    CHECK(bad([](AtmosphereSettings& a) { a.mie_g = -0.995f; }));
    CHECK(bad([](AtmosphereSettings& a) { a.sun_intensity = 0.0f; }));
    CHECK(bad([](AtmosphereSettings& a) { a.observer_height_km = -0.1f; }));
    CHECK(bad([](AtmosphereSettings& a) { a.observer_height_km = 60.0f; }));
    CHECK(bad([](AtmosphereSettings& a) { a.exposure = -1.0f; }));
    CHECK(!bad([](AtmosphereSettings& a) { a.mie_coefficient = 0.0f; a.exposure = 0.0f; }));
    // The functions refuse a zero or non-finite direction.
    bool threw = false;
    try {
        (void)sky_radiance(good, {0, 0, 0}, {0, 0, 1});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("Atmosphere: a high sun gives a blue sky with Rayleigh's color ratio, a halo around the sun, and a brighter horizon")
{
    const AtmosphereSettings a;
    const Float3 sun = dir_from(60.0, 0.0);
    const Float3 zenith = sky_radiance(a, {0, 0, 1}, sun);
    CHECK(zenith.z > zenith.y && zenith.y > zenith.x && zenith.x > 0.0f);
    // Rayleigh alone (no aerosol): blue over red is the ratio of the coefficients (5.7) less the extra attenuation of blue along the longer paths.
    AtmosphereSettings pure = a;
    pure.mie_coefficient = 0.0f;
    const Float3 clean = sky_radiance(pure, {0, 0, 1}, sun);
    const float ratio = clean.z / clean.x;
    CHECK(ratio > 3.5f && ratio < 5.7f);
    CHECK(zenith.z / zenith.x < ratio);  // aerosols scatter all colors alike, so they whiten the sky
    // The Mie halo: 5 degrees from the sun is much brighter than 60 degrees from it at the same elevation.
    const Float3 near_sun = sky_radiance(a, dir_from(60.0, 5.0 / std::cos(60.0 * deg)), sun);
    const Float3 far_sun = sky_radiance(a, dir_from(60.0, 180.0), sun);
    CHECK(near_sun.y > 3.0f * far_sun.y);
    // The horizon is brighter and whiter than the zenith (a longer path through more air), away from the sun.
    const Float3 horizon = sky_radiance(a, dir_from(2.0, 180.0), sun);
    CHECK(horizon.y > zenith.y);
    CHECK(horizon.x / horizon.z > zenith.x / zenith.z);
}

TEST_CASE("Atmosphere: sunlight reddens as the sun sets and is gone below the horizon")
{
    const AtmosphereSettings a;
    float previous_blue = 2.0f;
    for (const double elevation : {90.0, 60.0, 30.0, 15.0, 8.0, 4.0, 1.0}) {
        const Float3 t = sun_transmittance(a, dir_from(elevation, 0.0));
        CHECK(t.x >= t.y && t.y >= t.z);       // red passes best
        CHECK(t.z < previous_blue);            // and blue falls as the sun lowers
        CHECK(t.x > 0.0f && t.x <= 1.0f);
        previous_blue = t.z;
    }
    const Float3 zenith = sun_transmittance(a, {0, 0, 1}), horizon = sun_transmittance(a, dir_from(0.5, 0.0));
    CHECK(zenith.z > 0.6f && zenith.x > 0.9f);                 // overhead: the air costs a little blue, almost no red
    CHECK(horizon.x > 30.0f * horizon.z && horizon.x < 0.5f);  // at the horizon: red survives, blue is nearly gone
    const Float3 below = sun_transmittance(a, dir_from(-5.0, 0.0));
    CHECK(below.x == 0.0f && below.y == 0.0f && below.z == 0.0f);
    // A thinner atmosphere (no air) passes everything.
    AtmosphereSettings none = a;
    none.rayleigh_coefficient = {1e-9f, 1e-9f, 1e-9f};
    none.mie_coefficient = 0.0f;
    const Float3 clear = sun_transmittance(none, dir_from(30.0, 0.0));
    CHECK(clear.x > 0.999f && clear.z > 0.999f);
}

TEST_CASE("Atmosphere: the sky is finite and non-negative everywhere, dark at night, and linear in the sun's intensity")
{
    const AtmosphereSettings a;
    for (const double sun_elevation : {80.0, 30.0, 5.0, 0.0, -5.0, -20.0, -60.0}) {
        const Float3 sun = dir_from(sun_elevation, 30.0);
        for (double el = -90.0; el <= 90.0; el += 15.0) {
            for (double az = 0.0; az < 360.0; az += 45.0) {
                const Float3 c = sky_radiance(a, dir_from(el, az), sun);
                CHECK(finite3(c) && c.x >= 0.0f && c.y >= 0.0f && c.z >= 0.0f);
            }
        }
    }
    const Float3 noon = sky_radiance(a, {0, 0, 1}, dir_from(60.0, 0.0)), night = sky_radiance(a, {0, 0, 1}, dir_from(-20.0, 0.0));
    CHECK(night.y < 1e-3f * noon.y);  // the planet's shadow: no direct light reaches the air overhead
    AtmosphereSettings twice = a;
    twice.sun_intensity = 2.0f * a.sun_intensity;
    const Float3 doubled = sky_radiance(twice, {0, 0, 1}, dir_from(60.0, 0.0));
    CHECK(std::fabs(doubled.y - 2.0f * noon.y) < 1e-4f * noon.y);
    // Rotating the view and the sun together about the vertical axis changes nothing.
    const Float3 c1 = sky_radiance(a, dir_from(25.0, 10.0), dir_from(40.0, 70.0)), c2 = sky_radiance(a, dir_from(25.0, 110.0), dir_from(40.0, 170.0));
    CHECK(std::fabs(c1.x - c2.x) < 1e-5f * c1.x && std::fabs(c1.z - c2.z) < 1e-5f * c1.z);
}

TEST_CASE("Atmosphere: the horizon color follows the sun and the exposure")
{
    AtmosphereSettings a;
    const Float3 day = sky_horizon_color(a, dir_from(50.0, 0.0)), sunset = sky_horizon_color(a, dir_from(2.0, 0.0)), night = sky_horizon_color(a, dir_from(-30.0, 0.0));
    CHECK(day.y > 0.0f && finite3(day));
    CHECK(sunset.x / sunset.z > day.x / day.z);  // redder at sunset
    CHECK(night.y < 0.01f * day.y);
    a.exposure = 0.5f;
    const Float3 half = sky_horizon_color(a, dir_from(50.0, 0.0));
    CHECK(std::fabs(half.y - 0.5f * day.y) < 1e-5f * day.y);
}

TEST_CASE("Solar position: known configurations")
{
    // Equator at an equinox (day 80), solar noon: the sun is overhead (Cooper's declination is about -0.3 degrees on that day).
    const SolarPosition noon = solar_position(80, 12.0, 0.0);
    CHECK(noon.elevation / deg > 89.0);
    CHECK(noon.to_sun.z > 0.999f);
    // Winter solstice at 45 N, noon: 90 - 45 - 23.44 degrees high, due south (azimuth 180), and south is -X when +X is north.
    const SolarPosition winter = solar_position(355, 12.0, 45.0);
    CHECK(std::fabs(winter.elevation / deg - 21.56) < 0.7);
    CHECK(std::fabs(winter.azimuth / deg - 180.0) < 1.0);
    CHECK(winter.to_sun.x < -0.8f && winter.to_sun.z > 0.3f && std::fabs(winter.to_sun.y) < 0.05f);
    // Summer solstice at 45 N, noon: 68.44 degrees high.
    const SolarPosition summer = solar_position(172, 12.0, 45.0);
    CHECK(std::fabs(summer.elevation / deg - 68.44) < 0.7);
    // Equinox sunrise and sunset at the equator: on the horizon, due east and due west (east is -Y, west +Y).
    const SolarPosition sunrise = solar_position(80, 6.0, 0.0), sunset = solar_position(80, 18.0, 0.0);
    CHECK(std::fabs(sunrise.elevation / deg) < 1.0 && std::fabs(sunrise.azimuth / deg - 90.0) < 3.0);
    CHECK(sunrise.to_sun.y < -0.99f);
    CHECK(std::fabs(sunset.elevation / deg) < 1.0 && std::fabs(sunset.azimuth / deg - 270.0) < 3.0);
    CHECK(sunset.to_sun.y > 0.99f);
    // Midnight at 45 N: below the horizon.
    const SolarPosition midnight = solar_position(80, 0.0, 45.0);
    CHECK(midnight.elevation < 0.0 && midnight.to_sun.z < -0.5f);
    // Polar night and midnight sun.
    CHECK(solar_position(355, 12.0, 80.0).elevation < 0.0);
    CHECK(solar_position(172, 0.0, 80.0).elevation > 0.0);
    // Unit length, and the yaw turns the world about +Z: with north = +Y, east is +X, so the equinox sunrise is along +X.
    const SolarPosition turned = solar_position(80, 6.0, 0.0, 90.0 * deg);
    CHECK(turned.to_sun.x > 0.99f);
    for (const double hour : {0.0, 3.0, 9.5, 12.0, 17.25, 24.0}) {
        const Float3 v = solar_position(200, hour, 35.0, 0.4).to_sun;
        CHECK(std::fabs(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z) - 1.0f) < 1e-5f);
    }
    // Noon is the highest point of the day and the hours are symmetric about it.
    double best = -10.0;
    for (double hour = 0.0; hour <= 24.0; hour += 0.25) best = std::max(best, solar_position(120, hour, 40.0).elevation);
    CHECK(std::fabs(best - solar_position(120, 12.0, 40.0).elevation) < 1e-9);
    CHECK(std::fabs(solar_position(120, 9.0, 40.0).elevation - solar_position(120, 15.0, 40.0).elevation) < 1e-9);
    // Invalid input.
    const auto refused = [](int day, double hour, double lat, double yaw) {
        try {
            (void)solar_position(day, hour, lat, yaw);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(refused(0, 12.0, 0.0, 0.0) && refused(367, 12.0, 0.0, 0.0) && refused(100, -0.1, 0.0, 0.0) && refused(100, 24.5, 0.0, 0.0) && refused(100, 12.0, 91.0, 0.0) &&
          refused(100, nan, 0.0, 0.0) && refused(100, 12.0, nan, 0.0) && refused(100, 12.0, 0.0, nan));
}
