#include "saige/free_camera.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace saige {

void FreeCamera::update(CameraInput input, std::chrono::nanoseconds step) {
    const float seconds = std::chrono::duration<float>(step).count();
    if (seconds <= 0.F) return;
    constexpr float pi = std::numbers::pi_v<float>;
    constexpr float pitch_limit = pi * 0.5F - 0.01F;
    const auto finite = [](float value) { return std::isfinite(value) ? value : 0.F; };
    yaw_radians = std::remainder(yaw_radians - finite(input.look_x_pixels) * radians_per_pixel, 2.F * pi);
    pitch_radians = std::clamp(pitch_radians - finite(input.look_y_pixels) * radians_per_pixel,
                               -pitch_limit, pitch_limit);

    const float forward = std::clamp(finite(input.forward), -1.F, 1.F);
    const float right = std::clamp(finite(input.right), -1.F, 1.F);
    const float rise = std::clamp(finite(input.rise), -1.F, 1.F);
    const float cos_yaw = std::cos(yaw_radians);
    const float sin_yaw = std::sin(yaw_radians);
    const float projected = forward * std::cos(pitch_radians);
    const float dx = projected * cos_yaw + right * sin_yaw;
    const float dy = projected * sin_yaw - right * cos_yaw;
    const float dz = forward * std::sin(pitch_radians) + rise;
    const float distance = speed_mps * seconds / std::max(1.F, std::hypot(dx, dy, dz));
    position.x += distance * dx;
    position.y += distance * dy;
    position.z += distance * dz;
}

CameraView FreeCamera::view() const {
    const float sin_yaw = std::sin(yaw_radians * 0.5F);
    const float cos_yaw = std::cos(yaw_radians * 0.5F);
    const float sin_pitch = std::sin(pitch_radians * -0.5F);
    const float cos_pitch = std::cos(pitch_radians * 0.5F);
    return {position, {-sin_yaw * sin_pitch, cos_yaw * sin_pitch,
                       sin_yaw * cos_pitch, cos_yaw * cos_pitch}, fov_y_radians, near_plane};
}

} // namespace saige
