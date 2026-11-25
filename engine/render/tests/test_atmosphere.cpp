#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/atmosphere.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
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

// ---- Celestial bodies: sun disc, moon and stars -------------------------------------------------------------------------------------------------

namespace {

bool near_rel(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance * std::max(std::fabs(a), std::fabs(b)) + 1e-12f; }

AtmosphereSettings with_bodies()
{
    AtmosphereSettings a;
    a.sun_disc_intensity = 3000.0f;
    a.moon_intensity = 20.0f;
    a.star_intensity = 4.0f;
    return a;
}

}  // namespace

TEST_CASE("Celestial bodies: off by default, and the intensities are validated")
{
    const AtmosphereSettings off;
    const Float3 sun = dir_from(30.0, 0.0);
    CHECK(sun_disc_radiance(off, sun, sun).x == 0.0f);
    CHECK(moon_radiance(off, dir_from(-30.0, 180.0), sun).x == 0.0f);
    CHECK(star_radiance(off, dir_from(60.0, 10.0), dir_from(-40.0, 0.0)).x == 0.0f);
    const Float3 a = sky_with_bodies(off, dir_from(20.0, 50.0), sun), b = sky_radiance(off, dir_from(20.0, 50.0), sun);
    CHECK(a.x == b.x && a.y == b.y && a.z == b.z);  // bit for bit
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (float AtmosphereSettings::*field : {&AtmosphereSettings::sun_disc_intensity, &AtmosphereSettings::moon_intensity, &AtmosphereSettings::star_intensity}) {
        for (const float bad : {-1.0f, nan, 2.0e6f}) {
            AtmosphereSettings s;
            s.*field = bad;
            CHECK(throws_invalid(s));
        }
    }
    AtmosphereSettings edge;
    edge.sun_disc_intensity = 1.0e6f;
    validate_atmosphere(edge);
}

TEST_CASE("Sun disc: the disc is where the sun is, half a degree wide, limb-darkened with a soft edge, reddened by the air, and absent below the horizon")
{
    const AtmosphereSettings a = with_bodies();
    const Float3 high = dir_from(60.0, 20.0);
    const Float3 center = sun_disc_radiance(a, high, high);
    const Float3 transmittance = sun_transmittance(a, high);
    CHECK(near_rel(center.x, 3000.0f * transmittance.x, 1e-5f));  // the center: the full intensity times the air's transmittance (no limb darkening at the center)
    CHECK(center.z <= center.x);
    // Across the disc: monotone non-increasing from the center, 0 outside the radius, and symmetric.
    float previous = center.x;
    int inside = 0;
    for (int i = 0; i <= 40; ++i) {
        const double offset_deg = static_cast<double>(i) * 0.012;  // out to 0.48 degrees
        const float v = sun_disc_radiance(a, dir_from(60.0 + offset_deg, 20.0), high).x;
        CHECK(v <= previous + 1e-3f);
        previous = v;
        inside += v > 0.0f ? 1 : 0;
    }
    CHECK(inside >= 21 && inside <= 24);  // the disc's radius is 0.2664 degrees: 22 steps of 0.012 degrees, plus the center
    CHECK(sun_disc_radiance(a, dir_from(60.5, 20.0), high).x == 0.0f);   // 0.5 degrees away: outside the 0.27 degree radius
    CHECK(sun_disc_radiance(a, dir_from(59.8, 20.0), high).x > 0.0f);    // 0.2 degrees below the center: inside
    CHECK(near_rel(sun_disc_radiance(a, dir_from(59.8, 20.0), high).x, sun_disc_radiance(a, dir_from(60.2, 20.0), high).x, 2e-3f));  // symmetric about the center
    CHECK(sun_disc_radiance(a, dir_from(-60.0, 200.0), high).x == 0.0f);  // the opposite side of the sky
    const float limb = sun_disc_radiance(a, dir_from(60.0 + 0.2, 20.0), high).x;
    CHECK(limb > 0.3f * center.x);  // limb darkening, not a cutoff: the disc is still bright at 3/4 of its radius
    CHECK(limb < center.x);
    // Reddening: near the horizon the disc loses blue far faster than red.
    const Float3 low = dir_from(2.0, 20.0);
    const Float3 low_center = sun_disc_radiance(a, low, low);
    CHECK(low_center.x > 0.0f);
    CHECK(low_center.z / low_center.x < center.z / center.x);
    CHECK(low_center.x < center.x);
    // Below the horizon there is no disc (the sun is behind the planet), however bright.
    const Float3 set = dir_from(-3.0, 20.0);
    CHECK(sun_disc_radiance(a, set, set).x == 0.0f);
    CHECK(sun_disc_radiance(a, set, set).y == 0.0f);
}

TEST_CASE("Moon: opposite the sun, dimmer, with its own transmittance; and stars: deterministic, only at night, only above the horizon, a plausible sparse field")
{
    const AtmosphereSettings a = with_bodies();
    const Float3 sun = dir_from(-25.0, 40.0);
    const Float3 moon = dir_from(25.0, 220.0);  // exactly opposite
    CHECK(moon_radiance(a, moon, sun).x > 0.0f);
    CHECK(near_rel(moon_radiance(a, moon, sun).x, 20.0f * sun_transmittance(a, moon).x, 1e-5f));
    CHECK(moon_radiance(a, sun, sun).x == 0.0f);  // not where the sun is
    CHECK(moon_radiance(a, dir_from(25.0 + 0.3, 220.0), sun).x == 0.0f);
    // The moon follows the sun: when the sun is up, the moon is below the horizon, so nothing is drawn.
    const Float3 day_sun = dir_from(30.0, 40.0);
    CHECK(moon_radiance(a, dir_from(-30.0, 220.0), day_sun).x == 0.0f);

    // Stars: at night, above the horizon, deterministic.
    const Float3 night_sun = dir_from(-40.0, 0.0);
    int stars = 0, total = 0;
    float brightest = 0.0f;
    for (int i = 0; i < 40000; ++i) {
        // A low-discrepancy sweep of the upper hemisphere above 15 degrees (full horizon fade is past).
        const double u = (i + 0.5) / 40000.0, v = std::fmod(i * 0.6180339887498949, 1.0);
        const double z = 0.26 + 0.74 * u, phi = 6.283185307179586 * v, r = std::sqrt(1.0 - z * z);
        const Float3 d{static_cast<float>(r * std::cos(phi)), static_cast<float>(r * std::sin(phi)), static_cast<float>(z)};
        const Float3 s = star_radiance(a, d, night_sun);
        const Float3 again = star_radiance(a, d, night_sun);
        CHECK(s.x == again.x && s.y == again.y && s.z == again.z);
        CHECK(s.x >= 0.0f && s.y >= 0.0f && s.z >= 0.0f);
        CHECK(s.x <= 4.0f * 1.0001f);
        ++total;
        if (s.x > 0.0f || s.y > 0.0f || s.z > 0.0f) {
            ++stars;
            brightest = std::max(brightest, s.y);
        }
    }
    const float fraction = static_cast<float>(stars) / static_cast<float>(total);
    std::printf("  star pixels: %d of %d (%.3f%%), brightest %.2f\n", stars, total, static_cast<double>(fraction) * 100.0, static_cast<double>(brightest));
    CHECK(fraction > 0.0005f);  // a sparse field, not empty
    CHECK(fraction < 0.05f);    // and not a wall of dots
    CHECK(brightest > 1.0f);    // some stars are bright
    // No stars by day, or at the horizon, or below it; and they fade in as the sun sets.
    const Float3 up = dir_from(70.0, 10.0);
    const auto count_stars = [&](Float3 sun_dir, double elevation_deg) {
        int n = 0;
        for (int i = 0; i < 4000; ++i) {
            const Float3 d = dir_from(elevation_deg, i * 0.09);
            const Float3 s = star_radiance(a, d, sun_dir);
            n += (s.x > 0.0f || s.y > 0.0f || s.z > 0.0f) ? 1 : 0;
        }
        return n;
    };
    CHECK(star_radiance(a, up, dir_from(30.0, 0.0)).x == 0.0f);
    CHECK(star_radiance(a, up, dir_from(0.0, 0.0)).x == 0.0f);
    CHECK(count_stars(dir_from(30.0, 0.0), 60.0) == 0);
    CHECK(count_stars(night_sun, 60.0) > 0);
    CHECK(count_stars(night_sun, -10.0) == 0);   // below the horizon
    CHECK(count_stars(night_sun, 0.0) == 0);     // at it
    // The glow of a given star scales with how dark the sky is.
    float dusk_sum = 0.0f, dark_sum = 0.0f;
    for (int i = 0; i < 20000; ++i) {
        const Float3 d = dir_from(55.0, i * 0.018);
        dusk_sum += star_radiance(a, d, dir_from(-6.0, 0.0)).y;
        dark_sum += star_radiance(a, d, dir_from(-30.0, 0.0)).y;
    }
    CHECK(dark_sum > dusk_sum);
    CHECK(dusk_sum > 0.0f);
    // sky_with_bodies is the sum of its parts.
    const Float3 d = dir_from(25.0, 220.0);
    const Float3 total_radiance = sky_with_bodies(a, d, sun), parts_sky = sky_radiance(a, d, sun), parts_moon = moon_radiance(a, d, sun), parts_sun = sun_disc_radiance(a, d, sun),
                 parts_stars = star_radiance(a, d, sun);
    CHECK(near_rel(total_radiance.x, parts_sky.x + parts_moon.x + parts_sun.x + parts_stars.x, 1e-6f));
}
