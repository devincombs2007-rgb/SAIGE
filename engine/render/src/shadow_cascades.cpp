#include "saige/render/shadow_cascades.hpp"

#include <algorithm>
#include <cmath>

namespace saige::render {

std::array<float, kMaxShadowCascades + 1> cascade_splits(float n, float f, uint32_t count, float lambda) noexcept
{
    std::array<float, kMaxShadowCascades + 1> splits{};
    if (count == 0 || count > kMaxShadowCascades || !(n > 0.0f) || !(f > n) || !std::isfinite(n) || !std::isfinite(f)) {
        return splits;
    }
    const float l = std::clamp(lambda, 0.0f, 1.0f);
    for (uint32_t i = 0; i <= count; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(count);
        const float uniform = n + (f - n) * t;
        const float logarithmic = n * std::pow(f / n, t);
        splits[i] = uniform + (logarithmic - uniform) * l;
    }
    splits[0] = n;
    splits[count] = f;  // exact ends, whatever the rounding in between
    return splits;
}

SliceSphere slice_bounding_sphere(float n, float f, float fov_y_radians, float aspect) noexcept
{
    const float tan_half = std::tan(fov_y_radians * 0.5f);
    const float k2 = (1.0f + aspect * aspect) * tan_half * tan_half;  // squared ratio of a corner's offset from the axis to its depth
    // Equal distance to the nearest and farthest corners puts the center at (n + f)(1 + k^2) / 2 along the axis; when that lies beyond
    // the far plane (a very wide, short slice) the sphere is centered on the far plane's middle and passes through its corners.
    if (k2 >= (f - n) / (f + n)) {
        return {f, f * std::sqrt(k2)};
    }
    const float cz = 0.5f * (f + n) * (1.0f + k2);
    return {cz, std::sqrt((f - cz) * (f - cz) + f * f * k2)};
}

ShadowCascades fit_shadow_cascades(const CascadeCamera& camera, Float3 to_sun, const CascadeSettings& s) noexcept
{
    ShadowCascades out;
    const std::array<float, kMaxShadowCascades + 1> splits = cascade_splits(s.near_plane, s.shadow_distance, s.count, s.split_lambda);
    const float forward_len = length(camera.forward);
    const float sun_len = length(to_sun);
    if (splits[s.count > kMaxShadowCascades ? 0 : s.count] <= 0.0f || s.resolution == 0 || !(forward_len > 0.5f) || !(sun_len > 0.5f) ||
        !(camera.aspect > 0.0f) || !(camera.fov_y_radians > 0.0f) || !(camera.fov_y_radians < 3.1f) || !std::isfinite(forward_len) ||
        !std::isfinite(sun_len) || !std::isfinite(camera.position.x) || !std::isfinite(camera.position.y) || !std::isfinite(camera.position.z)) {
        return out;
    }
    const Float3 forward = camera.forward * (1.0f / forward_len);
    const Float3 sun = to_sun * (1.0f / sun_len);

    // The light's basis comes from the sun direction alone, so the shadow-map texel grid is fixed in the world whatever the camera does.
    const Float3 light_forward = sun * -1.0f;  // the direction light travels
    const Float3 helper = std::fabs(light_forward.z) > 0.99f ? Float3{1.0f, 0.0f, 0.0f} : Float3{0.0f, 0.0f, 1.0f};
    const Float3 right = normalize(cross(light_forward, helper));
    const Float3 up = cross(right, light_forward);

    out.count = s.count;
    for (uint32_t i = 0; i < s.count; ++i) {
        ShadowCascade& c = out.cascades[i];
        c.split_near = splits[i];
        c.split_far = splits[i + 1];
        const SliceSphere sphere = slice_bounding_sphere(c.split_near, c.split_far, camera.fov_y_radians, camera.aspect);
        c.radius = sphere.radius;
        c.texel_world_size = 2.0f * sphere.radius / static_cast<float>(s.resolution);
        Float3 center = camera.position + forward * sphere.axis_distance;
        if (s.stabilize) {
            // Snap the center to whole texels in light space (x and y; the depth axis does not shimmer).
            const float cx = dot(center, right), cy = dot(center, up);
            const float sx = std::round(cx / c.texel_world_size) * c.texel_world_size;
            const float sy = std::round(cy / c.texel_world_size) * c.texel_world_size;
            center = center + right * (sx - cx) + up * (sy - cy);
        }
        c.center = center;
        const float pull_back = sphere.radius + s.caster_margin;
        const Float3 eye = center + sun * pull_back;
        c.depth_range = 2.0f * sphere.radius + s.caster_margin;

        Mat4 view = Mat4::identity();  // rows: right, up, -forward; the eye at the origin
        view.at(0, 0) = right.x;
        view.at(0, 1) = right.y;
        view.at(0, 2) = right.z;
        view.at(0, 3) = -dot(right, eye);
        view.at(1, 0) = up.x;
        view.at(1, 1) = up.y;
        view.at(1, 2) = up.z;
        view.at(1, 3) = -dot(up, eye);
        view.at(2, 0) = -light_forward.x;
        view.at(2, 1) = -light_forward.y;
        view.at(2, 2) = -light_forward.z;
        view.at(2, 3) = dot(light_forward, eye);

        Mat4 proj;  // orthographic: x, y in [-radius, radius] to [-1, 1]; depth [0, range] along the light to 1 .. 0
        proj.at(0, 0) = 1.0f / sphere.radius;
        proj.at(1, 1) = 1.0f / sphere.radius;
        proj.at(2, 2) = 1.0f / c.depth_range;
        proj.at(2, 3) = 1.0f;
        proj.at(3, 3) = 1.0f;
        c.view_proj = proj * view;
    }
    return out;
}

uint32_t select_cascade(const ShadowCascades& cascades, float depth) noexcept
{
    for (uint32_t i = 0; i < cascades.count; ++i) {
        if (depth < cascades.cascades[i].split_far) {
            return i;
        }
    }
    return cascades.count;
}

} // namespace saige::render
