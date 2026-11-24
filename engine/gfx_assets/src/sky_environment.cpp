#include "saige/gfx_assets/sky_environment.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::gfx {

HdrImage sky_environment_image(const render::AtmosphereSettings& atmosphere, render::Float3 to_sun, const SkyEnvironmentSettings& s)
{
    render::validate_atmosphere(atmosphere);
    const auto finite = [](float v) { return std::isfinite(v); };
    if (s.source_width < 16 || s.source_width > 1024 || (s.source_width & 1u) != 0 || !finite(s.ground_albedo) || s.ground_albedo < 0.0f || s.ground_albedo > 1.0f ||
        !finite(s.night_ambient.x) || !finite(s.night_ambient.y) || !finite(s.night_ambient.z) || s.night_ambient.x < 0.0f || s.night_ambient.y < 0.0f || s.night_ambient.z < 0.0f ||
        !finite(s.sun_irradiance.x) || !finite(s.sun_irradiance.y) || !finite(s.sun_irradiance.z) || s.sun_irradiance.x < 0.0f || s.sun_irradiance.y < 0.0f || s.sun_irradiance.z < 0.0f) {
        throw std::invalid_argument("SkyEnvironmentSettings: the source width must be even and 16 to 1024, the albedo 0 to 1 and the sun irradiance and night ambient finite and not negative");
    }
    if (!finite(to_sun.x) || !finite(to_sun.y) || !finite(to_sun.z) || (to_sun.x == 0.0f && to_sun.y == 0.0f && to_sun.z == 0.0f)) {
        throw std::invalid_argument("sky_environment: the sun direction must be finite and not zero");
    }
    const render::Float3 sun = render::normalize(to_sun);
    HdrImage image;
    image.width = s.source_width;
    image.height = s.source_width / 2;
    image.rgb.assign(std::size_t{image.width} * image.height * 3, 0.0f);

    // How much of the night floor is in: 0 by day, smoothly up to 1 once the sun is about 8 degrees below the horizon.
    const float night_t = std::min(std::max((0.02f - sun.z) / 0.16f, 0.0f), 1.0f);
    const float night = night_t * night_t * (3.0f - 2.0f * night_t);
    // The sky above the horizon, and its cosine-weighted mean (the irradiance the ground sees from it, divided by pi).
    double weighted[3] = {0.0, 0.0, 0.0}, weight = 0.0;
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(image.width), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height);
            const render::Float3 d = equirect_direction(u, v);
            if (d.z < 0.0f) continue;
            const render::Float3 radiance = render::sky_radiance(atmosphere, d, sun);
            float* out = &image.rgb[(std::size_t{y} * image.width + x) * 3];
            out[0] = radiance.x * atmosphere.exposure + s.night_ambient.x * night;
            out[1] = radiance.y * atmosphere.exposure + s.night_ambient.y * night;
            out[2] = radiance.z * atmosphere.exposure + s.night_ambient.z * night;
            // Each texel covers a solid angle proportional to sin(theta) = sqrt(1 - z^2) (equirect), and the cosine weight is z.
            const double dw = static_cast<double>(d.z) * std::sqrt(std::max(0.0, 1.0 - static_cast<double>(d.z) * static_cast<double>(d.z)));
            weighted[0] += dw * static_cast<double>(out[0]);
            weighted[1] += dw * static_cast<double>(out[1]);
            weighted[2] += dw * static_cast<double>(out[2]);
            weight += dw;
        }
    }
    const render::Float3 mean = weight > 0.0 ? render::Float3{static_cast<float>(weighted[0] / weight), static_cast<float>(weighted[1] / weight), static_cast<float>(weighted[2] / weight)}
                                             : render::Float3{};
    const render::Float3 t = render::sun_transmittance(atmosphere, sun);
    const float lit = std::max(0.0f, sun.z);
    const render::Float3 ground{s.ground_albedo * (s.sun_irradiance.x * lit * t.x + mean.x), s.ground_albedo * (s.sun_irradiance.y * lit * t.y + mean.y),
                                s.ground_albedo * (s.sun_irradiance.z * lit * t.z + mean.z)};
    for (uint32_t y = 0; y < image.height; ++y) {
        for (uint32_t x = 0; x < image.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(image.width), v = (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height);
            if (equirect_direction(u, v).z >= 0.0f) continue;
            float* out = &image.rgb[(std::size_t{y} * image.width + x) * 3];
            out[0] = ground.x;
            out[1] = ground.y;
            out[2] = ground.z;
        }
    }
    return image;
}

EnvironmentResult sky_environment(const render::AtmosphereSettings& atmosphere, render::Float3 to_sun, const SkyEnvironmentSettings& settings)
{
    return precompute_environment(sky_environment_image(atmosphere, to_sun, settings), settings.filter);
}

} // namespace saige::gfx
