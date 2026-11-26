// Local lights (point and spot), CPU reference.
//
// What a local light is, in the units the engine already uses. A directional light's `color` is the radiance a white Lambertian surface facing it reflects (shaders/scene/pbr.hlsli: irradiance = color * PI). A local
// light is described by its irradiance on a surface that faces it squarely at one metre, `intensity` (the same quantity as `color * PI` for a directional light, so a point light of intensity PI at 1 m lights a
// white wall like a directional light of colour 1), times an RGB `color` for hue. A photometric authoring value converts to it: a source of luminous flux F lumens radiates F / (4 PI) candela (point) or
// F / (2 PI (1 - cos theta_outer)) candela (spot, theta_outer the cone's half angle), and 1 cd gives 1 lux at 1 m, so intensity in candela IS irradiance at 1 m in lux; the engine's exposure maps lux to display
// (the scene is scene-referred, as the sun is).
//
// Falloff: physically the inverse square of the distance, E = I / d^2. Real renderers cannot light everything in the world from every lamp, so the influence is windowed by a smooth function that reaches
// exactly zero at the light's `radius` with zero slope (no visible edge, and a light can be culled to a sphere of that radius): window(d) = saturate(1 - (d / R)^4)^2 (Karis, "Real Shading in Unreal Engine 4").
// The distance is clamped below at `min_distance` so a surface at the light's own position is finite. A spot adds an angular term: t = saturate((cos a - cos outer) / (cos inner - cos outer)), attenuation t^2
// (angles are half angles from the axis). Emissive materials are NOT lights: nothing here reads a material, and an emissive surface contributes to no other surface.
//
// This is the oracle the shaders and the GPU light culling will be compared against; it is not an optimised implementation and says nothing about GPU cost.
#pragma once

#include <array>

namespace saige::render_reference {

using Vec3 = std::array<double, 3>;

inline constexpr double kPi = 3.14159265358979323846;
/// Distances below this (metres) are treated as this, so 1 / d^2 stays finite on the light itself.
inline constexpr double kMinLightDistance = 0.01;

struct PointLight {
    Vec3 position{0, 0, 0};
    Vec3 color{1, 1, 1};     ///< linear RGB (hue; the strength is `intensity`)
    double intensity = 1.0;  ///< irradiance at 1 m on a surface facing the light (lux when authored in photometric units)
    double radius = 10.0;    ///< metres: the influence is exactly zero at and beyond it
};

struct SpotLight {
    Vec3 position{0, 0, 0};
    Vec3 direction{0, 0, -1};  ///< unit axis, the way the light shines
    Vec3 color{1, 1, 1};
    double intensity = 1.0;    ///< on the axis, as a point light's
    double radius = 10.0;
    double inner_angle = 0.3;  ///< half angle, radians: full strength inside
    double outer_angle = 0.5;  ///< half angle, radians: zero outside (0 < inner <= outer < pi/2 ... up to pi)
};

/// The luminous intensity (candela) of a point source of `lumens`: lumens / (4 pi).
[[nodiscard]] double point_intensity_from_flux(double lumens);
/// The luminous intensity (candela) on the axis of a spot of `lumens` spread evenly over its outer cone (half angle `outer_angle`): lumens / (2 pi (1 - cos outer)).
[[nodiscard]] double spot_intensity_from_flux(double lumens, double outer_angle);

/// 1 / max(d, min_distance)^2.
[[nodiscard]] double inverse_square(double distance, double min_distance = kMinLightDistance);
/// saturate(1 - (d / radius)^4)^2: 1 at the light, 0 (with zero slope) at `radius`, 0 beyond; 0 for a radius that is not positive and finite.
[[nodiscard]] double radius_window(double distance, double radius);
/// radius_window * inverse_square.
[[nodiscard]] double distance_attenuation(double distance, double radius);
/// The spot cone term for cosine `cos_angle` of the angle from the axis: 1 inside `inner`, 0 outside `outer`, t^2 between (angles are half angles; an inner angle above the outer is treated as equal to it).
[[nodiscard]] double spot_attenuation(double cos_angle, double inner_angle, double outer_angle);

/// One light's effect at a point: the unit direction from the point toward the light and the irradiance (RGB, on a surface facing the light squarely: multiply by n . l for a tilted one).
struct LightSample {
    Vec3 to_light{0, 0, 1};
    Vec3 irradiance{0, 0, 0};
};
/// False (and an unspecified sample) when the light does not reach the point: outside its radius, outside the cone, or an invalid light.
[[nodiscard]] bool sample_point_light(const PointLight& light, const Vec3& point, LightSample& out);
[[nodiscard]] bool sample_spot_light(const SpotLight& light, const Vec3& point, LightSample& out);

/// Radiance reflected by a white-or-coloured Lambertian surface (`albedo`, unit `normal`) lit by `sample`: albedo / pi * irradiance * max(n . l, 0).
[[nodiscard]] Vec3 shade_lambert(const Vec3& albedo, const Vec3& normal, const LightSample& sample);

} // namespace saige::render_reference
