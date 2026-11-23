#pragma once

#include "saige/capsule_character.hpp"
#include "saige/desktop.hpp"
#include "saige/free_camera.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>

namespace saige::sample {

class DesktopGame final : public IGame {
public:
    DesktopGame(World& world, IDesktopPlatform& platform, bool course,
                std::optional<std::span<const Aabb>> course_boxes = {})
        : marker_(world.create()), platform_(platform) {
        if (!course) return;
        constexpr std::array default_boxes{
            Aabb{{-32, -32, -1}, {32, 32, 0}},
            Aabb{{2.5f, -0.5f, 0}, {3.5f, 0.5f, 1}},
            Aabb{{4.5f, 0.5f, 0}, {5.5f, 1.5f, 1}},
            Aabb{{5.5f, -1.5f, 0}, {6.5f, -0.5f, 1}},
        };
        const auto geometry = course_boxes.value_or(std::span<const Aabb>(default_boxes));
        if (geometry.empty() || geometry.front().min.x >= 0 || geometry.front().max.x <= 0 ||
            geometry.front().min.y >= 0 || geometry.front().max.y <= 0 || geometry.front().max.z != 0) {
            throw std::invalid_argument("The first course box must be a floor with the origin inside its top face");
        }
        motor_.emplace(Vec3{}, geometry);
        for (const auto& box : geometry.subspan(1)) {
            (void)world.create({std::midpoint(box.min.x, box.max.x), std::midpoint(box.min.y, box.max.y), box.min.z});
        }
    }

    DesktopGame(World& world, IDesktopPlatform& platform, CapsuleCharacter controller)
        : marker_(world.create(controller.state().feet)), platform_(platform), motor_(std::move(controller)) {}

    [[nodiscard]] Vec3 course_position() const { return motor_.value().state().feet; }
    bool replace_course_collision(std::span<const Aabb> geometry) { return motor_.value().replace_static_geometry(geometry); }

    void fixed_update(World& world, std::chrono::nanoseconds step) override {
        const int right = platform_.key_down(Key::d) || platform_.key_down(Key::right);
        const int left = platform_.key_down(Key::a) || platform_.key_down(Key::left);
        const int forward = platform_.key_down(Key::w) || platform_.key_down(Key::up);
        const int back = platform_.key_down(Key::s) || platform_.key_down(Key::down);
        if (motor_) {
            const auto look = platform_.consume_look_delta();
            const bool jump = platform_.key_down(Key::space);
            if (!jump) suppress_jump_until_release_ = false;
            if (platform_.key_down(Key::r)) {
                reset_course(jump);
            } else {
                orbit_.update({.look_x_pixels = look.x_pixels, .look_y_pixels = look.y_pixels}, step);
                orbit_.pitch_radians = std::clamp(orbit_.pitch_radians, -1.25f, -0.16f);
                const float move_forward = static_cast<float>(forward - back);
                const float move_left = static_cast<float>(left - right);
                const float magnitude = std::max(1.0f, std::hypot(move_forward, move_left));
                const float cos_yaw = std::cos(orbit_.yaw_radians);
                const float sin_yaw = std::sin(orbit_.yaw_radians);
                motor_->step({(move_forward * cos_yaw - move_left * sin_yaw) / magnitude,
                              (move_forward * sin_yaw + move_left * cos_yaw) / magnitude,
                              jump && !suppress_jump_until_release_, platform_.key_down(Key::shift)}, step);
                if (motor_->state().feet.z < -8.0f) reset_course(jump);
            }
            world.set_position(marker_, motor_->state().feet);
            return;
        }
        const float seconds = std::chrono::duration<float>(step).count();
        // The marker drifts one world unit per second so unattended runs exercise simulation.
        position_.x += seconds * (1.0f + 4.0f * static_cast<float>(forward - back));
        position_.y += seconds * 4.0f * static_cast<float>(left - right);
        world.set_position(marker_, position_);
    }

    [[nodiscard]] std::optional<CameraView> camera_view() const override {
        if (!motor_) return {};
        const float projected = std::cos(orbit_.pitch_radians);
        const Vec3 direction{projected * std::cos(orbit_.yaw_radians),
                             projected * std::sin(orbit_.yaw_radians), std::sin(orbit_.pitch_radians)};
        constexpr float distance = 6.0f;
        const auto feet = motor_->state().feet;
        auto view = orbit_.view();
        view.position = {feet.x - direction.x * distance, feet.y - direction.y * distance,
                         feet.z + 1.0f - direction.z * distance};
        return view;
    }

private:
    void reset_course(bool jump_held) {
        motor_->reset();
        orbit_ = initial_orbit_;
        suppress_jump_until_release_ = jump_held;
    }

    static constexpr FreeCamera initial_orbit_{.pitch_radians = -0.35f};
    Entity marker_;
    IDesktopPlatform& platform_;
    Vec3 position_{};
    std::optional<CapsuleCharacter> motor_;
    FreeCamera orbit_{initial_orbit_};
    bool suppress_jump_until_release_{};
};

} // namespace saige::sample
