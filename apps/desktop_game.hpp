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
#include <numeric>
#include <optional>
#include <vector>

namespace saige::sample {

class DesktopGame final : public IGame {
public:
    DesktopGame(World& world, IDesktopPlatform& platform, bool course,
                std::optional<std::span<const Aabb>> course_boxes = {}, CapsuleCharacterConfig config = {})
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
        configure_course({}, geometry, config);
        for (const auto& box : geometry.subspan(1)) {
            (void)world.create({std::midpoint(box.min.x, box.max.x), std::midpoint(box.min.y, box.max.y), box.min.z});
        }
    }

    DesktopGame(World& world, IDesktopPlatform& platform, CapsuleCharacter controller,
                std::span<const Aabb> boxes, std::span<const Triangle> triangles)
        : marker_(world.create(controller.state().feet)), platform_(platform), motor_(std::move(controller)),
          camera_geometry_(boxes.begin(), boxes.end()), camera_triangles_(triangles.begin(), triangles.end()) {
        camera_distance_ = clear_camera_distance();
    }

    [[nodiscard]] Vec3 course_position() const { return motor_.value().state().feet; }
    bool replace_course_collision(std::span<const Aabb> boxes, std::span<const Triangle> triangles) {
        std::vector<Aabb> camera_boxes(boxes.begin(), boxes.end());
        std::vector<Triangle> camera_triangles(triangles.begin(), triangles.end());
        if (!motor_->replace_static_geometry(boxes, triangles)) return false;
        camera_geometry_.swap(camera_boxes);
        camera_triangles_.swap(camera_triangles);
        return true;
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
        const auto box = sweep_capsule_boxes(sphere, movement, camera_geometry_);
        const auto triangle = sweep_capsule_triangles(sphere, movement, camera_triangles_);
        const float box_t = box ? box->t : 1.0f;
        const float triangle_t = triangle ? triangle->t : 1.0f;
        return box || triangle ? std::max(0.0f, std::min(box_t, triangle_t) * orbit_distance - 0.03f) : orbit_distance;
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
    std::vector<Triangle> camera_triangles_;
    FreeCamera orbit_{initial_orbit_};
    float camera_distance_{orbit_distance};
    bool suppress_jump_until_release_{};
};

} // namespace saige::sample
