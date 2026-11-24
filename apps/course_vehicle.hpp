#pragma once

#include "course_input.hpp"
#include "saige/capsule_character.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <optional>
#include <span>

namespace saige::sample {

class CourseVehicle {
public:
    explicit CourseVehicle(Vec3 spawn) : spawn_(spawn), position_(spawn) {}

    void step(MovementActions input, bool brake, std::chrono::nanoseconds step, std::span<const Aabb> boxes,
              std::span<const Triangle> triangles = {}) {
        const float dt = std::chrono::duration<float>(step).count();
        const float target = brake ? 0.0f : input.forward * (input.forward >= 0 ? 10.0f : 4.0f);
        const float acceleration = brake ? 14.0f : input.forward == 0 ? 3.0f : 6.0f;
        speed_ += std::clamp(target - speed_, -dt * acceleration, dt * acceleration);
        const float steering = input.left * std::clamp(speed_ * 0.4f, -1.0f, 1.0f);
        heading_ = std::remainder(heading_ + steering * 1.3f * dt, 2.0f * std::numbers::pi_v<float>);
        const Vec3 move{std::cos(heading_) * speed_ * dt, std::sin(heading_) * speed_ * dt, 0};
        const Capsule body{position_, radius, 2.0f * radius};
        Vec3 candidate{position_.x + move.x, position_.y + move.y, position_.z};
        if (const auto hit = sweep(body, move, boxes, triangles)) {
            candidate = hit->stop;
            park();
        }
        // This prototype stays on its flat driving surface.
        if (!can_occupy(candidate, boxes, triangles)) {
            park();
            return;
        }
        position_ = candidate;
    }

    [[nodiscard]] bool can_enter(const CapsuleCharacterState& character, const CapsuleCharacterConfig& config,
                                 std::span<const Aabb> boxes, std::span<const Triangle> triangles = {}) const {
        const Vec3 approach{position_.x - character.feet.x, position_.y - character.feet.y, 0};
        return character.grounded && std::abs(character.feet.z - position_.z) < 0.25f &&
               std::hypot(approach.x, approach.y) <= 1.4f &&
               !sweep({character.feet, config.radius, config.height}, approach, boxes, triangles);
    }

    [[nodiscard]] std::optional<Vec3> exit_point(const CapsuleCharacterConfig& config,
                                                 std::span<const Aabb> boxes,
                                                 std::span<const Triangle> triangles = {}) const {
        if (std::abs(speed_) > 0.25f) return {};
        const Capsule rider{position_, config.radius, config.height};
        const Vec3 left{-std::sin(heading_), std::cos(heading_), 0};
        for (const float distance : {1.15f, -1.15f}) {
            const Vec3 move{left.x * distance, left.y * distance, 0};
            if (sweep(rider, move, boxes, triangles)) continue;
            const Capsule side{{position_.x + move.x, position_.y + move.y, position_.z},
                               rider.radius, rider.height};
            if (const auto floor = floor_below(side, boxes, triangles)) return floor->stop;
        }
        return {};
    }

    [[nodiscard]] bool can_occupy(Vec3 feet, std::span<const Aabb> boxes,
                                  std::span<const Triangle> triangles = {}) const {
        const Capsule body{feet, radius, 2.0f * radius};
        return std::ranges::none_of(boxes, [&](const auto& box) { return capsule_box_push_out(body, box).has_value(); }) &&
               std::ranges::none_of(triangles, [&](const auto& triangle) { return capsule_triangle_push_out(body, triangle).has_value(); }) &&
               floor_below(body, boxes, triangles).has_value();
    }

    [[nodiscard]] Vec3 reset_position() const noexcept { return spawn_; }

    void park() noexcept { speed_ = 0; }
    void reset() noexcept {
        position_ = spawn_;
        heading_ = 0;
        park();
    }
    [[nodiscard]] Vec3 position() const noexcept { return position_; }
    [[nodiscard]] float heading() const noexcept { return heading_; }
    [[nodiscard]] float speed() const noexcept { return speed_; }

private:
    [[nodiscard]] static std::optional<CapsuleHit> sweep(Capsule capsule, Vec3 movement,
                                                         std::span<const Aabb> boxes, std::span<const Triangle> triangles) {
        const auto box = sweep_capsule_boxes(capsule, movement, boxes);
        const auto triangle = sweep_capsule_triangles(capsule, movement, triangles);
        return triangle && (!box || triangle->contact_t < box->contact_t) ? triangle : box;
    }

    [[nodiscard]] static std::optional<CapsuleHit> floor_below(Capsule capsule, std::span<const Aabb> boxes,
                                                               std::span<const Triangle> triangles) {
        const auto hit = sweep(capsule, {0, 0, -0.08f}, boxes, triangles);
        return hit && !hit->started_overlapping && hit->normal.z > 0.99f ? hit : std::nullopt;
    }

    static constexpr float radius = 0.6f;
    Vec3 spawn_;
    Vec3 position_;
    float heading_{};
    float speed_{};
};

} // namespace saige::sample
