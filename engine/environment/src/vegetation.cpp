#include "saige/env/vegetation.hpp"

#include "saige/env/random.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige::env {

namespace {
constexpr double kTwoPi = 6.283185307179586;
}

double natural_frequency_hz(double height_m)
{
    if (!(height_m > 0.0) || !std::isfinite(height_m)) throw std::invalid_argument("natural_frequency_hz: the height must be positive and finite");
    return 0.5 / std::sqrt(height_m);
}

double sway_angle(const PlantWind& plant, double wind_speed, double t, double phase)
{
    if (!(plant.height_m > 0.0) || !(plant.stiffness > 0.0) || !(plant.max_angle > 0.0) || !(wind_speed >= 0.0) || !std::isfinite(wind_speed) || !std::isfinite(t) || !std::isfinite(phase))
        throw std::invalid_argument("sway_angle: the height, stiffness and maximum angle must be positive, the wind speed finite and >= 0, and the time and phase finite");
    // Dynamic pressure is proportional to the speed squared; tanh saturates the lean at max_angle. 12 m/s on a stiffness-1 plant reaches about 76% of it.
    const double load = (wind_speed * wind_speed) / (plant.stiffness * 140.0);
    const double lean = plant.max_angle * std::tanh(load);
    // The plant also rocks at its natural frequency, by up to a quarter of its lean, which is zero in still air.
    const double rock = 1.0 + 0.25 * std::sin(kTwoPi * natural_frequency_hz(plant.height_m) * t + phase);
    return lean * rock;
}

std::array<double, 3> bend_position(const std::array<double, 3>& local, double height_m, const std::array<double, 2>& wind_dir, double angle)
{
    if (!(height_m > 0.0) || !std::isfinite(angle)) throw std::invalid_argument("bend_position: the height must be positive and the angle finite");
    const double len = std::hypot(wind_dir[0], wind_dir[1]);
    if (!(len > 0.0)) return local;
    const double dx = wind_dir[0] / len, dy = wind_dir[1] / len;
    const double h = std::clamp(local[2] / height_m, 0.0, 1.0);
    const double a = angle * std::pow(h, 1.5);
    // Rotate about the axis k = up x wind_dir = (-dy, dx, 0): positive angles tip +z toward the wind direction (Rodrigues; k is perpendicular to up and horizontal).
    const double kx = -dy, ky = dx;
    const double c = std::cos(a), s = std::sin(a);
    const double vx = local[0], vy = local[1], vz = local[2];
    const double dot = kx * vx + ky * vy;
    const double cx = ky * vz;               // k x v
    const double cy = -kx * vz;
    const double cz = kx * vy - ky * vx;
    return {vx * c + cx * s + kx * dot * (1.0 - c), vy * c + cy * s + ky * dot * (1.0 - c), vz * c + cz * s};
}

std::array<double, 3> leaf_flutter(const std::array<double, 3>& normal, double mask, double wind_speed, double t, double phase)
{
    if (!(mask >= 0.0 && mask <= 1.0) || !(wind_speed >= 0.0) || !std::isfinite(wind_speed) || !std::isfinite(t) || !std::isfinite(phase))
        throw std::invalid_argument("leaf_flutter: the mask must be in [0, 1], the wind speed finite and >= 0, and the time and phase finite");
    const double amplitude = 0.03 * mask * std::min(wind_speed / 12.0, 1.0);
    const double wave = std::sin(kTwoPi * (2.0 + 0.3 * wind_speed) * t + phase);
    const double len = std::sqrt(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
    if (!(len > 0.0)) return {0.0, 0.0, 0.0};
    const double k = amplitude * wave / len;
    return {normal[0] * k, normal[1] * k, normal[2] * k};
}

namespace {

std::uint64_t cell_id(const GrassConfig& c, std::int64_t i, std::int64_t j)
{
    return hash_combine(hash_combine(c.seed, static_cast<std::uint64_t>(i)), static_cast<std::uint64_t>(j));
}

void validate(const GrassConfig& c)
{
    if (!(c.cell > 0.0) || !(c.jitter >= 0.0 && c.jitter <= 1.0) || !(c.near >= 0.0) || !(c.far > c.near) || !(c.min_fraction > 0.0 && c.min_fraction <= 1.0))
        throw std::invalid_argument("GrassConfig: the cell must be positive, the jitter in [0, 1], near >= 0, far > near and the minimum fraction in (0, 1]");
}

} // namespace

std::vector<GrassInstance> scatter_grass(const GrassConfig& c, double x0, double y0, double x1, double y1, const std::function<double(double, double)>& density)
{
    validate(c);
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1) || !(x1 > x0) || !(y1 > y0)) throw std::invalid_argument("scatter_grass: the region must be finite with x1 > x0 and y1 > y0");
    std::vector<GrassInstance> out;
    const std::int64_t i0 = static_cast<std::int64_t>(std::floor(x0 / c.cell)) - 1, i1 = static_cast<std::int64_t>(std::floor(x1 / c.cell)) + 1;
    const std::int64_t j0 = static_cast<std::int64_t>(std::floor(y0 / c.cell)) - 1, j1 = static_cast<std::int64_t>(std::floor(y1 / c.cell)) + 1;
    for (std::int64_t j = j0; j <= j1; ++j) {
        for (std::int64_t i = i0; i <= i1; ++i) {
            const std::uint64_t id = cell_id(c, i, j);
            const double x = (static_cast<double>(i) + 0.5 + (unit(hash_combine(id, 1)) - 0.5) * c.jitter) * c.cell;
            const double y = (static_cast<double>(j) + 0.5 + (unit(hash_combine(id, 2)) - 0.5) * c.jitter) * c.cell;
            if (x < x0 || x >= x1 || y < y0 || y >= y1) continue;
            if (density) {
                const double d = std::clamp(density(x, y), 0.0, 1.0);
                if (!(unit(hash_combine(id, 3)) < d)) continue;
            }
            out.push_back({x, y, kTwoPi * unit(hash_combine(id, 4)), 0.7 + 0.6 * unit(hash_combine(id, 5)), id});
        }
    }
    return out;
}

double lod_fraction(const GrassConfig& c, double distance)
{
    validate(c);
    if (!std::isfinite(distance) || distance < 0.0) throw std::invalid_argument("lod_fraction: the distance must be finite and >= 0");
    if (distance <= c.near) return 1.0;
    const double t = std::clamp((distance - c.near) / (c.far - c.near), 0.0, 1.0);
    const double s = t * t * (3.0 - 2.0 * t);
    return 1.0 + (c.min_fraction - 1.0) * s;
}

bool lod_keep(const GrassConfig& c, const GrassInstance& instance, double distance)
{
    return unit(hash_combine(instance.id, 6)) < lod_fraction(c, distance);
}

double lod_width_scale(const GrassConfig& c, double distance) { return 1.0 / std::sqrt(lod_fraction(c, distance)); }

} // namespace saige::env
