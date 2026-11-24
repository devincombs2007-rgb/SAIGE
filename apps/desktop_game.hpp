#pragma once

#include "course_input.hpp"
#include "saige/capsule_character.hpp"
#include "saige/desktop.hpp"
#include "saige/free_camera.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace saige::sample {

class DesktopGame final : public IGame {
public:
    DesktopGame(World& world, IDesktopPlatform& platform, bool course,
                CapsuleCharacterConfig config = {})
        : marker_(world.create()), platform_(platform) {
        if (!course) return;
        constexpr std::array geometry{
            Aabb{{-32, -32, -1}, {32, 32, 0}},
            Aabb{{2.5f, -0.5f, 0}, {3.5f, 0.5f, 1}},
            Aabb{{4.5f, 0.5f, 0}, {5.5f, 1.5f, 1}},
            Aabb{{5.5f, -1.5f, 0}, {6.5f, -0.5f, 1}},
        };
        configure_course({}, geometry, config);
        for (std::size_t i = 1; i < geometry.size(); ++i) {
            const auto& box = geometry[i];
            (void)world.create({(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, box.min.z});
        }
    }

    void fixed_update(World& world, std::chrono::nanoseconds step) override {
        if (motor_) {
            const auto actions = read_course_actions(platform_);
            if (!actions.jump) suppress_jump_until_release_ = false;
            if (actions.reset) {
                reset_course(actions.jump);
            } else {
                orbit_.update({.look_x_pixels = actions.look.x_pixels, .look_y_pixels = actions.look.y_pixels}, step);
                orbit_.pitch_radians = std::clamp(orbit_.pitch_radians, -1.25f, -0.16f);
                const auto [forward, left] = actions.movement;
                const float magnitude = std::max(1.0f, std::hypot(forward, left));
                const float cos_yaw = std::cos(orbit_.yaw_radians);
                const float sin_yaw = std::sin(orbit_.yaw_radians);
                motor_->step({(forward * cos_yaw - left * sin_yaw) / magnitude,
                              (forward * sin_yaw + left * cos_yaw) / magnitude,
                              actions.jump && !suppress_jump_until_release_, actions.sprint}, step);
                if (motor_->state().feet.z < -8.0f) reset_course(actions.jump);
            }
            update_camera_distance(step);
            world.set_position(marker_, motor_->state().feet);
            return;
        }
        const auto movement = read_movement(platform_);
        const float seconds = std::chrono::duration<float>(step).count();
        // The marker drifts one world unit per second so unattended runs exercise simulation.
        position_.x += seconds * (1.0f + 4.0f * movement.forward);
        position_.y += seconds * 4.0f * movement.left;
        world.set_position(marker_, position_);
    }

    [[nodiscard]] std::optional<CameraView> camera_view() const override {
        if (!motor_) return {};
        const auto direction = orbit_direction();
        const auto target = orbit_target();
        auto view = orbit_.view();
        view.position = {target.x - direction.x * camera_distance_, target.y - direction.y * camera_distance_,
                         target.z - direction.z * camera_distance_};
        return view;
    }

private:
    void configure_course(Vec3 spawn, std::span<const Aabb> geometry, const CapsuleCharacterConfig& config) {
        motor_.emplace(spawn, geometry, config);
        camera_geometry_.assign(geometry.begin(), geometry.end());
        camera_distance_ = clear_camera_distance();
    }

    [[nodiscard]] Vec3 orbit_direction() const {
        const float horizontal = std::cos(orbit_.pitch_radians);
        return {horizontal * std::cos(orbit_.yaw_radians), horizontal * std::sin(orbit_.yaw_radians),
                std::sin(orbit_.pitch_radians)};
    }

    [[nodiscard]] Vec3 orbit_target() const {
        const auto feet = motor_->state().feet;
        return {feet.x, feet.y, feet.z + motor_->config().height * 0.58f};
    }

    [[nodiscard]] float clear_camera_distance() const {
        const auto target = orbit_target();
        const auto direction = orbit_direction();
        constexpr float radius = 0.12f;
        const Capsule sphere{{target.x, target.y, target.z - radius}, radius, 2.0f * radius};
        const Vec3 movement{-direction.x * orbit_distance, -direction.y * orbit_distance, -direction.z * orbit_distance};
        if (const auto hit = sweep_capsule_boxes(sphere, movement, camera_geometry_)) {
            return std::max(0.0f, hit->t * orbit_distance - 0.03f);
        }
        return orbit_distance;
    }

    void update_camera_distance(std::chrono::nanoseconds step) {
        constexpr float extension_speed = 12.0f;
        const float permitted = clear_camera_distance();
        const float extension = extension_speed * std::chrono::duration<float>(step).count();
        camera_distance_ = std::min(permitted, camera_distance_ + extension);
    }

    void reset_course(bool jump_held) {
        motor_->reset();
        orbit_ = initial_orbit_;
        suppress_jump_until_release_ = jump_held;
        camera_distance_ = clear_camera_distance();
    }

    static constexpr float orbit_distance = 6.0f;
    static constexpr FreeCamera initial_orbit_{.pitch_radians = -0.35f};
    Entity marker_;
    IDesktopPlatform& platform_;
    Vec3 position_{};
    std::optional<CapsuleCharacter> motor_;
    std::vector<Aabb> camera_geometry_;
    FreeCamera orbit_{initial_orbit_};
    float camera_distance_{orbit_distance};
    bool suppress_jump_until_release_{};
};

} // namespace saige::sample
