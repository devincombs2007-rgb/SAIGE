#include "saige/render/atmosphere.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace saige::render {
namespace {

using D3 = std::array<double, 3>;

constexpr double pi_d = 3.14159265358979323846;

D3 to_d3(Float3 v) noexcept { return {v.x, v.y, v.z}; }
double dot(const D3& a, const D3& b) noexcept { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
D3 normalized(const D3& v) {
    const double len = std::sqrt(dot(v, v));
    if (!(len > 0.0) || !std::isfinite(len)) throw std::invalid_argument("atmosphere: a direction must be finite and non-zero");
    return {v[0] / len, v[1] / len, v[2] / len};
}

/// Near and far distances along o + t d (d unit) to a sphere of radius r at the origin; false when it misses.
bool sphere_hits(const D3& o, const D3& d, double r, double& t0, double& t1) noexcept {
    const double b = dot(o, d), c = dot(o, o) - r * r;
    const double disc = b * b - c;
    if (disc < 0.0) return false;
    const double s = std::sqrt(disc);
    t0 = -b - s;
    t1 = -b + s;
    return true;
}

/// The settings widened to double once, so that no expression mixes the two.
struct Params {
    double radius, top, observer, hr, hm, beta_m, g, intensity, exposure;
    D3 beta_r;
    explicit Params(const AtmosphereSettings& a) noexcept
        : radius(static_cast<double>(a.planet_radius_km)),
          top(static_cast<double>(a.planet_radius_km) + static_cast<double>(a.atmosphere_height_km)),
          observer(static_cast<double>(a.observer_height_km)),
          hr(static_cast<double>(a.rayleigh_scale_height_km)),
          hm(static_cast<double>(a.mie_scale_height_km)),
          beta_m(static_cast<double>(a.mie_coefficient)),
          g(static_cast<double>(a.mie_g)),
          intensity(static_cast<double>(a.sun_intensity)),
          exposure(static_cast<double>(a.exposure)),
          beta_r{static_cast<double>(a.rayleigh_coefficient.x), static_cast<double>(a.rayleigh_coefficient.y), static_cast<double>(a.rayleigh_coefficient.z)}
    {
    }
};

} // namespace

void validate_atmosphere(const AtmosphereSettings& a) {
    const auto finite = [](float v) { return std::isfinite(v); };
    const bool ok = finite(a.planet_radius_km) && finite(a.atmosphere_height_km) && finite(a.rayleigh_scale_height_km) && finite(a.mie_scale_height_km) &&
                    finite(a.rayleigh_coefficient.x) && finite(a.rayleigh_coefficient.y) && finite(a.rayleigh_coefficient.z) && finite(a.mie_coefficient) &&
                    finite(a.mie_g) && finite(a.sun_intensity) && finite(a.observer_height_km) && finite(a.exposure);
    if (!ok) throw std::invalid_argument("AtmosphereSettings values must be finite");
    if (!(a.planet_radius_km > 0.0f) || !(a.atmosphere_height_km > 0.0f) || !(a.rayleigh_scale_height_km > 0.0f) || !(a.mie_scale_height_km > 0.0f) ||
        !(a.rayleigh_coefficient.x > 0.0f) || !(a.rayleigh_coefficient.y > 0.0f) || !(a.rayleigh_coefficient.z > 0.0f) || a.mie_coefficient < 0.0f ||
        !(a.sun_intensity > 0.0f) || a.exposure < 0.0f) {
        throw std::invalid_argument("AtmosphereSettings: radii, heights, scale heights, the Rayleigh coefficients and the sun intensity must be positive; the Mie coefficient and exposure not negative");
    }
    if (a.mie_g < -0.99f || a.mie_g > 0.99f) throw std::invalid_argument("AtmosphereSettings: the Mie anisotropy must be within -0.99 to 0.99");
    for (const float v : {a.sun_disc_intensity, a.moon_intensity, a.star_intensity}) {
        if (!std::isfinite(v) || v < 0.0f || v > 1.0e6f) throw std::invalid_argument("AtmosphereSettings: the sun disc, moon and star intensities must be finite and in 0 to 1e6");
    }
    if (a.observer_height_km < 0.0f || a.observer_height_km >= a.atmosphere_height_km) {
        throw std::invalid_argument("AtmosphereSettings: the observer must be above the ground and below the top of the atmosphere");
    }
}

Float3 sky_radiance(const AtmosphereSettings& a, Float3 view_dir, Float3 to_sun_in) {
    const D3 d = normalized(to_d3(view_dir)), sun = normalized(to_d3(to_sun_in));
    const Params k(a);
    const double radius = k.radius, top = k.top, hr = k.hr, hm = k.hm, beta_m = k.beta_m;
    const D3 origin{0.0, 0.0, radius + k.observer};
    const D3& beta_r = k.beta_r;

    double t0 = 0.0, t1 = 0.0;
    if (!sphere_hits(origin, d, top, t0, t1)) return {};
    double t_max = t1;
    double p0 = 0.0, p1 = 0.0;
    if (sphere_hits(origin, d, radius, p0, p1) && p0 > 0.0) t_max = std::min(t_max, p0);  // the planet blocks the ray
    if (!(t_max > 0.0)) return {};

    const double segment = t_max / kAtmosphereViewSteps;
    double od_r = 0.0, od_m = 0.0;
    D3 sum_r{0, 0, 0}, sum_m{0, 0, 0};
    for (uint32_t i = 0; i < kAtmosphereViewSteps; ++i) {
        const double t = (i + 0.5) * segment;
        const D3 p{origin[0] + d[0] * t, origin[1] + d[1] * t, origin[2] + d[2] * t};
        const double height = std::sqrt(dot(p, p)) - radius;
        const double dr = std::exp(-height / hr) * segment, dm = std::exp(-height / hm) * segment;
        od_r += dr;
        od_m += dm;
        // The light path from here to the top of the atmosphere in the sun's direction; a point whose sun ray meets the planet is in its shadow.
        double l0 = 0.0, l1 = 0.0;
        if (!sphere_hits(p, sun, top, l0, l1)) continue;
        double s0 = 0.0, s1 = 0.0;
        if (sphere_hits(p, sun, radius, s0, s1) && s0 > 0.0) continue;
        const double light_segment = l1 / kAtmosphereLightSteps;
        double lod_r = 0.0, lod_m = 0.0;
        for (uint32_t j = 0; j < kAtmosphereLightSteps; ++j) {
            const double u = (j + 0.5) * light_segment;
            const D3 q{p[0] + sun[0] * u, p[1] + sun[1] * u, p[2] + sun[2] * u};
            const double qh = std::sqrt(dot(q, q)) - radius;
            lod_r += std::exp(-qh / hr) * light_segment;
            lod_m += std::exp(-qh / hm) * light_segment;
        }
        for (std::size_t c = 0; c < 3; ++c) {
            const double tau = beta_r[c] * (od_r + lod_r) + 1.1 * beta_m * (od_m + lod_m);  // Mie extinction is 1.1 times its scattering
            const double attenuation = std::exp(-tau);
            sum_r[c] += attenuation * dr;
            sum_m[c] += attenuation * dm;
        }
    }
    const double mu = dot(d, sun), g = k.g;
    const double phase_r = 3.0 / (16.0 * pi_d) * (1.0 + mu * mu);
    const double phase_m = 3.0 / (8.0 * pi_d) * ((1.0 - g * g) * (1.0 + mu * mu)) / ((2.0 + g * g) * std::pow(1.0 + g * g - 2.0 * g * mu, 1.5));
    Float3 out;
    out.x = static_cast<float>(k.intensity * (sum_r[0] * beta_r[0] * phase_r + sum_m[0] * beta_m * phase_m));
    out.y = static_cast<float>(k.intensity * (sum_r[1] * beta_r[1] * phase_r + sum_m[1] * beta_m * phase_m));
    out.z = static_cast<float>(k.intensity * (sum_r[2] * beta_r[2] * phase_r + sum_m[2] * beta_m * phase_m));
    return out;
}

Float3 sun_transmittance(const AtmosphereSettings& a, Float3 to_sun_in) {
    const D3 sun = normalized(to_d3(to_sun_in));
    const Params k(a);
    const double radius = k.radius, top = k.top;
    const D3 origin{0.0, 0.0, radius + k.observer};
    double s0 = 0.0, s1 = 0.0;
    if (sphere_hits(origin, sun, radius, s0, s1) && s0 > 0.0) return {};  // the sun is below the planet's horizon
    double t0 = 0.0, t1 = 0.0;
    if (!sphere_hits(origin, sun, top, t0, t1)) return {1.0f, 1.0f, 1.0f};
    constexpr uint32_t steps = 64;  // more than the in-scatter integral uses: this is a CPU-only helper
    const double segment = t1 / steps;
    double od_r = 0.0, od_m = 0.0;
    for (uint32_t i = 0; i < steps; ++i) {
        const double t = (i + 0.5) * segment;
        const D3 p{origin[0] + sun[0] * t, origin[1] + sun[1] * t, origin[2] + sun[2] * t};
        const double h = std::sqrt(dot(p, p)) - radius;
        od_r += std::exp(-h / k.hr) * segment;
        od_m += std::exp(-h / k.hm) * segment;
    }
    const auto channel = [&](double beta) { return static_cast<float>(std::exp(-(beta * od_r + 1.1 * k.beta_m * od_m))); };
    return {channel(k.beta_r[0]), channel(k.beta_r[1]), channel(k.beta_r[2])};
}

namespace {

/// A disc of angular radius `radius` around the unit vector `center`, seen along the unit vector `d`: sin(theta) is used rather than acos(dot), which has no precision left at half a degree.
float disc_profile(const D3& d, const D3& center, float radius) {
    if (dot(d, center) <= 0.0) return 0.0f;
    const D3 c{d[1] * center[2] - d[2] * center[1], d[2] * center[0] - d[0] * center[2], d[0] * center[1] - d[1] * center[0]};
    const float sin_theta = static_cast<float>(std::sqrt(dot(c, c)));
    const float x = sin_theta / std::sin(radius);
    if (!(x < 1.0f)) return 0.0f;
    const float mu = std::sqrt(std::max(0.0f, 1.0f - x * x));
    const float limb = 1.0f - 0.6f * (1.0f - mu);
    const float t = std::min(std::max((x - 0.85f) / 0.15f, 0.0f), 1.0f);
    return limb * (1.0f - t * t * (3.0f - 2.0f * t));
}

uint32_t hash_u(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
float u01(uint32_t h) { return static_cast<float>(h >> 8) * (1.0f / 16777216.0f); }

}  // namespace

Float3 sun_disc_radiance(const AtmosphereSettings& a, Float3 view_dir, Float3 to_sun_in) {
    if (!(a.sun_disc_intensity > 0.0f)) return {};
    const D3 d = normalized(to_d3(view_dir)), sun = normalized(to_d3(to_sun_in));
    const float profile = disc_profile(d, sun, kSunAngularRadius);
    if (!(profile > 0.0f)) return {};
    const Float3 t = sun_transmittance(a, to_sun_in);
    const float k = a.sun_disc_intensity * profile;
    return {t.x * k, t.y * k, t.z * k};
}

Float3 moon_radiance(const AtmosphereSettings& a, Float3 view_dir, Float3 to_sun_in) {
    if (!(a.moon_intensity > 0.0f)) return {};
    const D3 d = normalized(to_d3(view_dir)), sun = normalized(to_d3(to_sun_in));
    const D3 moon{-sun[0], -sun[1], -sun[2]};
    const float profile = disc_profile(d, moon, kMoonAngularRadius);
    if (!(profile > 0.0f)) return {};
    const Float3 t = sun_transmittance(a, Float3{static_cast<float>(moon[0]), static_cast<float>(moon[1]), static_cast<float>(moon[2])});
    const float k = a.moon_intensity * profile;
    return {t.x * k, t.y * k, t.z * k};
}

Float3 star_radiance(const AtmosphereSettings& a, Float3 view_dir, Float3 to_sun_in) {
    if (!(a.star_intensity > 0.0f)) return {};
    const D3 dd = normalized(to_d3(view_dir)), sd = normalized(to_d3(to_sun_in));
    const float dz = static_cast<float>(dd[2]), sun_z = static_cast<float>(sd[2]);
    const float night_t = std::min(std::max((sun_z + 0.0175f) / -0.1745f, 0.0f), 1.0f);  // 1 degree below the horizon begins, 11 degrees is full dark (sin of the angles, close enough)
    const float night = night_t * night_t * (3.0f - 2.0f * night_t);
    const float up_t = std::min(std::max(dz / 0.2f, 0.0f), 1.0f);
    const float horizon = up_t * up_t * (3.0f - 2.0f * up_t);
    if (!(night > 0.0f) || !(horizon > 0.0f)) return {};
    const float px = static_cast<float>(dd[0]) * 64.0f, py = static_cast<float>(dd[1]) * 64.0f, pz = static_cast<float>(dd[2]) * 64.0f;
    const int32_t ix = static_cast<int32_t>(std::floor(px)), iy = static_cast<int32_t>(std::floor(py)), iz = static_cast<int32_t>(std::floor(pz));
    const uint32_t h = hash_u(static_cast<uint32_t>(ix) ^ hash_u((static_cast<uint32_t>(iy) + 0x9e3779b9U) ^ hash_u(static_cast<uint32_t>(iz) + 0x85ebca6bU)));
    if (!(u01(h) < 0.5f)) return {};
    const float cx = static_cast<float>(ix) + 0.2f + 0.6f * u01(hash_u(h + 1u)), cy = static_cast<float>(iy) + 0.2f + 0.6f * u01(hash_u(h + 2u)),
                cz = static_cast<float>(iz) + 0.2f + 0.6f * u01(hash_u(h + 3u));
    const float dist = std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy) + (pz - cz) * (pz - cz));
    const float falloff = std::max(0.0f, 1.0f - dist / 0.12f);
    if (!(falloff > 0.0f)) return {};
    const float b = u01(hash_u(h + 4u)), tint = u01(hash_u(h + 5u));
    const float level = a.star_intensity * night * horizon * (0.3f + 0.7f * b * b) * falloff * falloff;
    return {level * (1.0f + (0.7f - 1.0f) * tint), level * (0.85f + (0.8f - 0.85f) * tint), level * (0.7f + (1.0f - 0.7f) * tint)};
}

Float3 sky_with_bodies(const AtmosphereSettings& a, Float3 view_dir, Float3 to_sun) {
    const Float3 sky = sky_radiance(a, view_dir, to_sun), sun = sun_disc_radiance(a, view_dir, to_sun), moon = moon_radiance(a, view_dir, to_sun), stars = star_radiance(a, view_dir, to_sun);
    return {sky.x + sun.x + moon.x + stars.x, sky.y + sun.y + moon.y + stars.y, sky.z + sun.z + moon.z + stars.z};
}

Float3 sky_horizon_color(const AtmosphereSettings& a, Float3 to_sun) {
    const double elevation = 3.0 * pi_d / 180.0;
    double r = 0.0, g = 0.0, b = 0.0;
    for (int i = 0; i < 8; ++i) {
        const double az = 2.0 * pi_d * i / 8.0;
        const Float3 dir{static_cast<float>(std::cos(elevation) * std::cos(az)), static_cast<float>(std::cos(elevation) * std::sin(az)), static_cast<float>(std::sin(elevation))};
        const Float3 c = sky_radiance(a, dir, to_sun);
        r += static_cast<double>(c.x);
        g += static_cast<double>(c.y);
        b += static_cast<double>(c.z);
    }
    const double scale = Params(a).exposure / 8.0;
    return {static_cast<float>(r * scale), static_cast<float>(g * scale), static_cast<float>(b * scale)};
}

SolarPosition solar_position(int day_of_year, double local_solar_hour, double latitude_degrees, double north_yaw_radians) {
    if (day_of_year < 1 || day_of_year > 366 || !std::isfinite(local_solar_hour) || local_solar_hour < 0.0 || local_solar_hour > 24.0 || !std::isfinite(latitude_degrees) ||
        latitude_degrees < -90.0 || latitude_degrees > 90.0 || !std::isfinite(north_yaw_radians)) {
        throw std::invalid_argument("solar_position: the day must be 1 to 366, the hour 0 to 24, the latitude -90 to 90 and the yaw finite");
    }
    const double rad = pi_d / 180.0;
    const double declination = 23.44 * rad * std::sin(2.0 * pi_d * (284.0 + day_of_year) / 365.0);
    const double hour_angle = 15.0 * rad * (local_solar_hour - 12.0);
    const double lat = latitude_degrees * rad;
    const double sin_el = std::sin(lat) * std::sin(declination) + std::cos(lat) * std::cos(declination) * std::cos(hour_angle);
    const double elevation = std::asin(std::clamp(sin_el, -1.0, 1.0));
    // Azimuth from north toward east: east component -cos(dec) sin(H), north component sin(dec) cos(lat) - cos(dec) sin(lat) cos(H).
    const double east = -std::cos(declination) * std::sin(hour_angle);
    const double north = std::sin(declination) * std::cos(lat) - std::cos(declination) * std::sin(lat) * std::cos(hour_angle);
    double azimuth = std::atan2(east, north);
    if (azimuth < 0.0) azimuth += 2.0 * pi_d;
    SolarPosition out;
    out.elevation = elevation;
    out.azimuth = azimuth;
    // World: +X is north rotated by the yaw, east is to the right of it (-Y when the yaw is 0, since +Y is the engine's left).
    const double nx = std::cos(north_yaw_radians), ny = std::sin(north_yaw_radians);
    const double ex = std::sin(north_yaw_radians), ey = -std::cos(north_yaw_radians);
    const double horizontal = std::cos(elevation);
    out.to_sun = {static_cast<float>(horizontal * (nx * std::cos(azimuth) + ex * std::sin(azimuth))), static_cast<float>(horizontal * (ny * std::cos(azimuth) + ey * std::sin(azimuth))),
                  static_cast<float>(std::sin(elevation))};
    return out;
}

} // namespace saige::render
