#pragma once

#include "saige/runtime.hpp"

namespace saige {

struct CameraInput {
    float forward{};
    float right{};
    float rise{};
    float look_x_pixels{};
    float look_y_pixels{};
};

struct FreeCamera {
    Vec3 position{};
    float yaw_radians{};
    float pitch_radians{};
    float speed_mps{4.0F};
    float radians_per_pixel{0.003F};
    float fov_y_radians{1.04719755F};
    float near_plane{0.1F};

    void update(CameraInput input, std::chrono::nanoseconds step);
    [[nodiscard]] CameraView view() const;
};

} // namespace saige
