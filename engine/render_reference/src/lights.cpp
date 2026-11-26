#include "saige/render_reference/lights.hpp"

#include <algorithm>
#include <cmath>

namespace saige::render_reference {

namespace {

double saturate(double x) { return std::clamp(x, 0.0, 1.0); }
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
bool finite3(const Vec3& v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

} // namespace

double point_intensity_from_flux(double lumens)
{
    return lumens / (4.0 * kPi);
}

double spot_intensity_from_flux(double lumens, double outer_angle)
{
    const double solid_angle = 2.0 * kPi * (1.0 - std::cos(outer_angle));
    return solid_angle > 0.0 ? lumens / solid_angle : 0.0;
}

double inverse_square(double distance, double min_distance)
{
    const double d = std::max(distance, min_distance);
    return 1.0 / (d * d);
}

double radius_window(double distance, double radius)
{
    if (!std::isfinite(distance) || !std::isfinite(radius) || !(radius > 0.0)) return 0.0;
    const double x = distance / radius;
    const double x2 = x * x;
    const double w = saturate(1.0 - x2 * x2);
    return w * w;
}

double distance_attenuation(double distance, double radius)
{
    return radius_window(distance, radius) * inverse_square(distance);
}

double spot_attenuation(double cos_angle, double inner_angle, double outer_angle)
{
    const double outer = std::max(outer_angle, 0.0);
    const double inner = std::min(std::max(inner_angle, 0.0), outer);
    const double cos_outer = std::cos(outer), cos_inner = std::cos(inner);
    const double scale = 1.0 / std::max(cos_inner - cos_outer, 1e-4);
    const double t = saturate((cos_angle - cos_outer) * scale);
    return t * t;
}

bool sample_point_light(const PointLight& light, const Vec3& point, LightSample& out)
{
    if (!finite3(light.position) || !finite3(light.color) || !finite3(point) || !std::isfinite(light.intensity) || !(light.intensity >= 0.0) || !(light.radius > 0.0) || !std::isfinite(light.radius)) return false;
    const Vec3 d{light.position[0] - point[0], light.position[1] - point[1], light.position[2] - point[2]};
    const double dist = std::sqrt(dot(d, d));
    if (!(dist < light.radius)) return false;
    const double inv = dist > 1e-12 ? 1.0 / dist : 0.0;
    out.to_light = dist > 1e-12 ? Vec3{d[0] * inv, d[1] * inv, d[2] * inv} : Vec3{0, 0, 1};
    const double e = light.intensity * distance_attenuation(dist, light.radius);
    out.irradiance = {light.color[0] * e, light.color[1] * e, light.color[2] * e};
    return true;
}

bool sample_spot_light(const SpotLight& light, const Vec3& point, LightSample& out)
{
    const double axis_len = std::sqrt(dot(light.direction, light.direction));
    if (!finite3(light.direction) || !(std::fabs(axis_len - 1.0) < 1e-3) || !(light.outer_angle > 0.0) || !(light.outer_angle <= kPi)) return false;
    PointLight p;
    p.position = light.position;
    p.color = light.color;
    p.intensity = light.intensity;
    p.radius = light.radius;
    if (!sample_point_light(p, point, out)) return false;
    // The angle from the axis to the point, seen from the light: the direction from the light to the point is -to_light.
    const double cos_angle = -dot(out.to_light, light.direction);
    const double cone = spot_attenuation(cos_angle, light.inner_angle, light.outer_angle);
    if (!(cone > 0.0)) return false;
    out.irradiance = {out.irradiance[0] * cone, out.irradiance[1] * cone, out.irradiance[2] * cone};
    return true;
}

Vec3 shade_lambert(const Vec3& albedo, const Vec3& normal, const LightSample& sample)
{
    const double n_dot_l = std::max(dot(normal, sample.to_light), 0.0);
    const double k = n_dot_l / kPi;
    return {albedo[0] * sample.irradiance[0] * k, albedo[1] * sample.irradiance[1] * k, albedo[2] * sample.irradiance[2] * k};
}

} // namespace saige::render_reference
